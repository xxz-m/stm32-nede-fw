/*============================================================================
* store.c —— 应用层：AO_Store 实现（掉电参数存储）
*
* 层次定位：App 层。只调用 Service（svc_param）与 Driver（drv_at24c04），
*           不碰 HAL、不碰 I2C 寄存器。
*
* 状态机（每个相位都在 AO 的 10 ms 节拍或 I2C 完成事件里往前推一小步）：
*
*   DISABLED ──────────────────────────────── 存储功能关闭（只有默认值）
*   PROBE ──有应答──> HDR(A) ──> HDR(B) ──> LOAD ──> READY
*     └──无应答───────────────────────────────────────> READY（用默认值）
*   READY ──dirty──> SAVE ──> VERIFY ──一致──> READY（清 dirty、版本 +1）
*     │                ^          │
*     │                └──不一致/失败──┘（重写，最多 APP_CFG_STORE_RETRY 次）
*     └──scan 请求──> SCAN ──> READY
*
* 关键设计：
*   1. **影子是运行期唯一真相**。EEPROM 只在启动读一次、修改时写一次；
*      读参数永远走内存，因此不会因为总线慢而拖住调用方。
*   2. **双槽交替 + 序号 + CRC**。先写「不是当前那份」的槽，写完回读比对，
*      成功才切换。写到一半掉电时旧记录仍然完好——这是 512 B 器件上最划算的保险。
*   3. **一次只干一件事**。任何时刻只允许一个底层作业在飞；相位机保证不会并发。
*===========================================================================*/
#include "store.h"
#include "app_config.h"
#include "app_prio.h"
#include "app_signals.h"
#include "bsp.h"
#include "drv_at24c04.h"
#include "svc_log.h"

#include <stdio.h>
#include <string.h>

Q_DEFINE_THIS_MODULE("store")
void Q_onAssert(char const * const module, int loc);

/*--------------------------------------------------------------------------*/
/* 相位定义                                                                  */
/*--------------------------------------------------------------------------*/
typedef enum {
    ST_DISABLED = 0,    /*!< APP_CFG_STORE_ENABLE=0：只保留影子，不碰总线 */
    ST_PROBE,           /*!< 探测介质是否存在                            */
    ST_HDR,             /*!< 读两个槽的记录头（sub=0 → A，1 → B）        */
    ST_LOAD,            /*!< 读选中槽的整条记录                          */
    ST_READY,           /*!< 就绪：等 dirty / 扫描请求                   */
    ST_SAVE,            /*!< 编码并写入备用槽                            */
    ST_VERIFY,          /*!< 回读并逐字节比对                            */
    ST_SCAN             /*!< I2C 地址扫描（诊断）                        */
} StorePhase;

/*! 槽序号（0=A，1=B）。 */
#define STORE_SLOT_A    (0U)
#define STORE_SLOT_B    (1U)
#define STORE_SLOT_NONE (0xFFU)

/*! 每拍最多探测几个 I2C 地址。单次探测约 90 µs（最坏 APP_CFG_STORE_PROBE_TIMEOUT_MS），
* 4 个 ≈ 0.4 ms，落在 AO 的单次预算（约 1 ms）之内；全量扫描 112 个地址因此需要
* 约 28 拍（≈ 0.3 s）。 */
#define STORE_SCAN_BURST    (4U)

/*! 扫描结果最多上报几台设备（`+I2C:<地址>` URC 条数上限）。
* 全量扫描 112 个地址，坏板子上可能每个地址都「应答」；再往后加一个上限，
* 免得把 512 B 的日志发送环冲爆（冲爆的后果是连 `+I2C:END` 都丢掉）。 */
#define STORE_SCAN_URC_MAX  (8U)

/*--------------------------------------------------------------------------*/
/* AO 实例                                                                   */
/*--------------------------------------------------------------------------*/
typedef struct {
    QActive super;                 /*!< 基类，必须放在首位 */
    QTimeEvt tick;                 /*!< 存储状态机的节拍   */
    SvcParam param;                /*!< 参数影子（运行期唯一真相） */

    uint8_t  phase;                /*!< StorePhase         */
    uint8_t  sub;                  /*!< ST_HDR 的子步      */
    uint8_t  attempts;             /*!< 当前相位已重试次数 */
    bool     waiting;              /*!< 已发起底层作业，等结果 */

    bool     present;              /*!< 介质应答过          */
    bool     resolved;             /*!< 启动加载阶段已结束  */
    bool     scanReq;              /*!< 有扫描请求未处理    */

    uint8_t  curSlot;              /*!< 当前记录所在槽      */
    bool     slotOk[2];            /*!< 各槽头部是否有效    */
    uint8_t  slotSeq[2];
    uint16_t slotVer[2];
    uint16_t slotRecLen[2];

    uint16_t readAddr;
    uint16_t readLen;
    uint8_t  writeSlot;
    uint16_t writeAddr;
    uint8_t  writeSeq;
    uint16_t writeLen;
    uint16_t nextVer;

    uint8_t  scanAddr;
    uint8_t  scanCount;

    uint32_t saved;
    uint32_t failed;

    /* NOTE (editSeq 的用处)：保存是异步的，编码发生在发起 ST_SAVE 的那一刻。
    * 如果用户在写入过程中又改了一次参数，那次修改不在本条记录里，**不能**在
    * 校验成功后把 dirty 清掉——否则新值永远不落盘（表现为「改了、回了 ok、
    * 重启却是旧值」）。所以这里记两个序号：每次改动 ++editSeq，发起保存时把
    * 当时的 editSeq 存进 saveEditSeq，只有两者相等才允许清 dirty。 */
    uint32_t editSeq;
    uint32_t saveEditSeq;

    uint8_t  hdr[SVC_PARAM_HDR_SIZE];
    uint8_t  rec[SVC_PARAM_RECORD_MAX];
    uint8_t  vfy[SVC_PARAM_RECORD_MAX];
} Store;

static Store l_store;
QActive * const AO_Store = &l_store.super;
static QEvt const *l_queue[8];

/* 前置声明：状态处理函数与相互引用的相位函数（Q_TRAN 需要符号先可见）。 */
static QState active(Store * const me, QEvt const * const e);
static void storePickSlot(Store *me);
static void storeFail(Store *me, drv_status_t err);

/* 节拍换算：APP_CFG_STORE_PROCESS_PERIOD_MS 毫秒 → QP 节拍数 */
#define STORE_TICKS ((APP_CFG_STORE_PROCESS_PERIOD_MS * BSP_TICKS_PER_SEC) / 1000U)

/*--------------------------------------------------------------------------*/
/* 小工具                                                                    */
/*--------------------------------------------------------------------------*/
static char slotChar(uint8_t slot) {
    return (slot == STORE_SLOT_A) ? 'A' : 'B';
}

/*..........................................................................*/
static uint16_t slotAddr(uint8_t slot) {
    return (slot == STORE_SLOT_A) ? (uint16_t)APP_CFG_STORE_SLOT_A_ADDR
                                  : (uint16_t)APP_CFG_STORE_SLOT_B_ADDR;
}

/*..........................................................................*/
static char const *svcErrStr(svc_status_t st) {
    switch (st) {
        case SVC_OK:         return "ok";
        case SVC_ERR_PARAM:  return "param";
        case SVC_ERR_RANGE:  return "range";
        case SVC_ERR_CRC:    return "crc";
        case SVC_ERR_STATE:  return "state";
        case SVC_ERR_NOMEM:  return "nomem";
        default:             return "?";
    }
}

/*..........................................................................*/
/** @brief 切到新相位：清等待标志与重试计数。 */
static void storeGoto(Store *me, uint8_t phase) {
    me->phase = phase;
    me->waiting = false;
    me->attempts = 0U;
}

/*..........................................................................*/
/** @brief 投一次「有新活干」的通知（让 AO 的下一次处理不等节拍）。 */
static void storePostCmd(void) {
    static QEvt const evt = { (QSignal)STORE_CMD_SIG, 0U, 0U };

    (void)QACTIVE_POST_X(AO_Store, &evt, 0U, 0U);
}

/*..........................................................................*/
/** @brief 上一拍发起的底层作业是否已结束。
* @param ok [out] 结束时填入成功 / 失败
* @return false = 还在进行（继续等）
*/
static bool busSettled(bool *ok) {
    switch (DRV_AT24C04_State()) {
        case DRV_AT24C04_STATE_DONE:
            *ok = true;
            return true;
        case DRV_AT24C04_STATE_FAILED:
            *ok = false;
            return true;
        default:
            return false;
    }
}

/*--------------------------------------------------------------------------*/
/* 失败与选槽                                                                */
/*--------------------------------------------------------------------------*/
/** @brief 放弃当前相位后的收尾处理（重试用尽才走到这里）。

* 每个相位的「放弃」语义不同：探测失败 = 器件不在；槽头/整条读不出来 =
* 该槽作废、换另一个槽；写入失败 = 这次修改只留在内存里（不再无限重试）。
*/
static void storeGiveUp(Store *me, drv_status_t err) {
    char const *why = DRV_AT24C04_ErrStr(err);

    switch (me->phase) {
        case ST_PROBE:
            me->present = false;
            me->resolved = true;        /* 介质不在：不再等，直接用编译期默认值 */
            LOG_W("store: no eeprom at 0x%02X (%s), using built-in defaults",
                  (unsigned)APP_CFG_STORE_I2C_ADDR, why);
            storeGoto(me, ST_READY);
            break;

        case ST_HDR:
            /* 某个槽的头读不出来：当作该槽无效，另一个槽照常参与选择 */
            me->slotOk[me->sub] = false;
            if (me->sub == STORE_SLOT_A) {
                me->sub = STORE_SLOT_B;
                storeGoto(me, ST_HDR);
            }
            else {
                storePickSlot(me);      /* 两个头都不可用 → 内部落到默认值 */
            }
            break;

        case ST_LOAD:
            ++me->failed;
            /* 这一槽读不出来：先看另一个槽，两个都不行才回落到默认值 */
            me->slotOk[me->curSlot] = false;
            me->curSlot = STORE_SLOT_NONE;
            LOG_W("store: slot read failed (%s), trying the other", why);
            storePickSlot(me);
            break;

        case ST_SAVE:
        case ST_VERIFY:
            ++me->failed;
            /* 不再无限重试：这次修改留在 RAM 里，但不写介质。
            * NOTE (为什么不保持 dirty)：保持 dirty 会让每个节拍都重试一次，
            * 介质真坏时会变成永久刷屏 + 一直占着总线。 */
            me->param.dirty = false;
            LOG_E("store: save failed (%s), change kept in RAM only", why);
            storeGoto(me, ST_READY);
            break;

        default:
            LOG_W("store: scan aborted (%s)", why);
            me->scanReq = false;
            storeGoto(me, ST_READY);
            break;
    }
}

/*..........................................................................*/
/** @brief 相位失败：先重试，用尽才放弃。
*
* NOTE (SAVE/VERIFY 的重试方向)：VERIFY 不一致说明**写**没成功，重读没有意义，
* 因此这两种相位统一回到 ST_SAVE 重写同一槽（换一个新的槽序号），再验一次。
*/
static void storeFail(Store *me, drv_status_t err) {
    me->waiting = false;
    if (me->attempts < APP_CFG_STORE_RETRY) {
        ++me->attempts;
        if ((me->phase == ST_SAVE) || (me->phase == ST_VERIFY)) {
            me->writeSeq = (uint8_t)(me->param.slot_seq + 1U);
            me->phase = ST_SAVE;    /* 重写而不是重读 */
        }
        /* 其余相位：停在本相位，下一拍重新发起（天然退避一个节拍） */
        return;
    }
    storeGiveUp(me, err);
}

/*..........................................................................*/
/** @brief 从两个槽的有效头里挑最新的一份，转到 ST_LOAD；都不行则用默认值。 */
static void storePickSlot(Store *me) {
    uint8_t pick;

    if (me->slotOk[STORE_SLOT_A] && me->slotOk[STORE_SLOT_B]) {
        pick = SVC_Param_SeqNewer(me->slotSeq[STORE_SLOT_B], me->slotSeq[STORE_SLOT_A])
                   ? STORE_SLOT_B : STORE_SLOT_A;
    }
    else if (me->slotOk[STORE_SLOT_A]) {
        pick = STORE_SLOT_A;
    }
    else if (me->slotOk[STORE_SLOT_B]) {
        pick = STORE_SLOT_B;
    }
    else {
        LOG_I("store: blank media, using built-in defaults");
        me->resolved = true;
        storeGoto(me, ST_READY);
        return;
    }

    me->curSlot = pick;
    me->readAddr = slotAddr(pick);
    me->readLen = me->slotRecLen[pick];
    storeGoto(me, ST_LOAD);
}

/*--------------------------------------------------------------------------*/
/* 各相位的推进                                                              */
/*--------------------------------------------------------------------------*/
static void storeRunProbe(Store *me) {
    bool ok;

    if (!me->waiting) {
        drv_status_t st = DRV_AT24C04_ProbeStart();
        if (st == DRV_OK) {
            me->waiting = true;
        }
        else if (st != DRV_ERR_BUSY) {
            storeFail(me, st);
        }
        return;
    }
    if (!busSettled(&ok)) {
        return;
    }
    if (ok) {
        me->present = true;
        LOG_I("store: eeprom at 0x%02X ready, %u B, page %u B",
              (unsigned)APP_CFG_STORE_I2C_ADDR,
              (unsigned)DRV_AT24C04_Capacity(),
              (unsigned)DRV_AT24C04_PageSize());
        me->sub = STORE_SLOT_A;
        storeGoto(me, ST_HDR);
    }
    else {
        storeFail(me, DRV_AT24C04_Result());
    }
}

/*..........................................................................*/
static void storeRunHdr(Store *me) {
    bool ok;

    if (!me->waiting) {
        me->readAddr = slotAddr(me->sub);
        me->readLen = (uint16_t)SVC_PARAM_HDR_SIZE;
        drv_status_t st = DRV_AT24C04_ReadStart(me->readAddr, me->hdr, me->readLen);
        if (st == DRV_OK) {
            me->waiting = true;
        }
        else if (st != DRV_ERR_BUSY) {
            storeFail(me, st);
        }
        return;
    }
    if (!busSettled(&ok)) {
        return;
    }

    me->slotOk[me->sub] = false;
    if (ok) {
        SvcParamHeader h;
        if (SVC_Param_ParseHeader(me->hdr, (size_t)SVC_PARAM_HDR_SIZE, &h)) {
            me->slotOk[me->sub] = true;
            me->slotSeq[me->sub] = h.slot_seq;
            me->slotVer[me->sub] = h.config_version;
            me->slotRecLen[me->sub] = h.record_len;
        }
    }
    else {
        /* 该槽读失败：记无效，继续看另一个槽（失败原因不再单独上报） */
        LOG_D("store: slot %c header unreadable (%s)",
              slotChar(me->sub), DRV_AT24C04_ErrStr(DRV_AT24C04_Result()));
    }

    me->waiting = false;
    if (me->sub == STORE_SLOT_A) {
        me->sub = STORE_SLOT_B;
        me->attempts = 0U;          /* B 槽有它自己的重试预算 */
        me->phase = ST_HDR;
    }
    else {
        storePickSlot(me);
    }
}

/*..........................................................................*/
static void storeRunLoad(Store *me) {
    bool ok;

    if (!me->waiting) {
        drv_status_t st = DRV_AT24C04_ReadStart(me->readAddr, me->rec, me->readLen);
        if (st == DRV_OK) {
            me->waiting = true;
        }
        else if (st != DRV_ERR_BUSY) {
            storeFail(me, st);
        }
        return;
    }
    if (!busSettled(&ok)) {
        return;
    }
    me->waiting = false;

    if (!ok) {
        storeFail(me, DRV_AT24C04_Result());
        return;
    }

    {
        svc_status_t const st = SVC_Param_Decode(&me->param, me->rec, (size_t)me->readLen);
        if (st != SVC_OK) {
            /* 头看着没问题、整条却解不开：多半是写入过程中掉电。换另一个槽再试。 */
            LOG_W("store: slot %c record bad (%s), trying the other",
                  slotChar(me->curSlot), svcErrStr(st));
            me->slotOk[me->curSlot] = false;
            me->curSlot = STORE_SLOT_NONE;
            storePickSlot(me);
            return;
        }
    }

    me->param.from_media = true;
    me->param.dirty = false;
    me->resolved = true;
    LOG_I("store: loaded ver=%u seq=%u slot=%c",
          (unsigned)me->param.config_version,
          (unsigned)me->param.slot_seq,
          slotChar(me->curSlot));
    storeGoto(me, ST_READY);
}

/*..........................................................................*/
static void storeRunReady(Store *me) {
    if (me->scanReq) {
        me->scanAddr = (uint8_t)APP_CFG_STORE_SCAN_FIRST;
        me->scanCount = 0U;
        storeGoto(me, ST_SCAN);
        return;
    }
    if (!me->param.dirty) {
        return;                     /* 没活干：本拍什么都不做 */
    }
    if (!me->present) {
        me->param.dirty = false;    /* 介质不在：不再反复尝试，避免刷屏 */
        LOG_W("store: change kept in RAM only (no eeprom)");
        return;
    }

    /* 交替写「另一个槽」：即使写到一半掉电，当前这份旧记录仍然完好可读 */
    me->writeSlot = (me->curSlot == STORE_SLOT_A) ? STORE_SLOT_B : STORE_SLOT_A;
    me->writeAddr = slotAddr(me->writeSlot);
    me->writeSeq = (uint8_t)(me->param.slot_seq + 1U);
    me->nextVer = (uint16_t)(me->param.config_version + 1U);
    me->saveEditSeq = me->editSeq;   /* 记住本条记录是「哪一次改动」的 */
    storeGoto(me, ST_SAVE);
}

/*..........................................................................*/
static void storeRunSave(Store *me) {
    bool ok;

    if (!me->waiting) {
        /* 编码要求缓冲能放下最坏情况（见 SVC_Param_Encode 的说明） */
        if (sizeof(me->rec) < (size_t)SVC_PARAM_RECORD_MAX) {
            storeFail(me, DRV_ERR_PARAM);
            return;
        }
        me->writeLen = (uint16_t)SVC_Param_Encode(&me->param, me->nextVer, me->writeSeq,
                                                  me->rec, sizeof(me->rec));
        if (me->writeLen == 0U) {
            storeFail(me, DRV_ERR_PARAM);
            return;
        }
        drv_status_t st = DRV_AT24C04_WriteStart(me->writeAddr, me->rec, me->writeLen);
        if (st == DRV_OK) {
            me->waiting = true;
        }
        else if (st != DRV_ERR_BUSY) {
            storeFail(me, st);
        }
        return;
    }
    if (!busSettled(&ok)) {
        return;
    }
    if (ok) {
        me->waiting = false;
        storeGoto(me, ST_VERIFY);   /* 写完必须回读比对，不能只看「发完了」 */
    }
    else {
        storeFail(me, DRV_AT24C04_Result());
    }
}

/*..........................................................................*/
static void storeRunVerify(Store *me) {
    bool ok;

    if (!me->waiting) {
        drv_status_t st = DRV_AT24C04_ReadStart(me->writeAddr, me->vfy, me->writeLen);
        if (st == DRV_OK) {
            me->waiting = true;
        }
        else if (st != DRV_ERR_BUSY) {
            storeFail(me, st);
        }
        return;
    }
    if (!busSettled(&ok)) {
        return;
    }
    me->waiting = false;

    if (!ok || (memcmp(me->rec, me->vfy, (size_t)me->writeLen) != 0)) {
        /* 注意：回读不一致 = 写失败，交给 storeFail 重写（它会换一个槽序号） */
        LOG_W("store: verify mismatch on slot %c, rewriting", slotChar(me->writeSlot));
        storeFail(me, ok ? DRV_ERR_BUS : DRV_AT24C04_Result());
        return;
    }

    /* 只有到这里才算真的保存成功 */
    me->param.config_version = me->nextVer;
    me->param.slot_seq = me->writeSeq;
    me->param.from_media = true;
    /* 若这期间用户又改过一次，dirty 必须保持为真，让 READY 再存一遍 */
    me->param.dirty = (me->editSeq != me->saveEditSeq);
    me->curSlot = me->writeSlot;
    me->slotOk[me->writeSlot] = true;
    me->slotSeq[me->writeSlot] = me->writeSeq;
    me->slotVer[me->writeSlot] = me->nextVer;
    me->slotRecLen[me->writeSlot] = me->writeLen;
    ++me->saved;

    LOG_I("store: saved ver=%u seq=%u slot=%c %u B",
          (unsigned)me->nextVer, (unsigned)me->writeSeq,
          slotChar(me->writeSlot), (unsigned)me->writeLen);
    storeGoto(me, ST_READY);
}

/*..........................................................................*/
static void storeRunScan(Store *me) {
    uint8_t burst = 0U;

    while ((me->scanAddr <= (uint8_t)APP_CFG_STORE_SCAN_LAST) && (burst < STORE_SCAN_BURST)) {
        uint8_t const addr = me->scanAddr;
        bsp_status_t const st = BSP_i2cProbe((uint8_t)(addr << 1),
                                             (uint32_t)APP_CFG_STORE_PROBE_TIMEOUT_MS);
        if (st == BSP_ERR_BUSY) {
            return;                 /* 总线忙：同一地址下拍再试 */
        }
        ++me->scanAddr;
        ++burst;
        if (st == BSP_OK) {
            ++me->scanCount;
            /* 结果以 URC 逐条上报（AT+I2C? 已经先回过 OK 表示「已受理」）。
            * NOTE (为什么用 LOG_WRITE 而不是 LOG_I)：LOG_I 会加 "I/" 前缀，
            *   那样就不是 AT 响应了；URC 必须原样出现在串口上。 */
            if (me->scanCount <= STORE_SCAN_URC_MAX) {
                char urc[16];
                (void)snprintf(urc, sizeof(urc), "+I2C:%02X\r\n", (unsigned)addr);
                LOG_WRITE(urc);
            }
        }
    }

    if (me->scanAddr <= (uint8_t)APP_CFG_STORE_SCAN_LAST) {
        return;                     /* 还没扫完：下拍继续 */
    }

    /* 结束标记：上行工具靠它判断扫描收尾（否则只能等超时） */
    LOG_WRITE("+I2C:END\r\n");
    LOG_I("store: i2c scan done, %u device(s)", (unsigned)me->scanCount);
    me->scanReq = false;
    storeGoto(me, ST_READY);
}

/*--------------------------------------------------------------------------*/
/* 主推进                                                                    */
/*--------------------------------------------------------------------------*/
static void storeProcess(Store *me) {
    if (me->phase == ST_DISABLED) {
        return;
    }

    /* 先把底层作业往前推：它负责页拆分、写周期等待与重试 */
    DRV_AT24C04_Process();

    switch (me->phase) {
        case ST_PROBE:  storeRunProbe(me);  break;
        case ST_HDR:    storeRunHdr(me);    break;
        case ST_LOAD:   storeRunLoad(me);   break;
        case ST_READY:  storeRunReady(me);  break;
        case ST_SAVE:   storeRunSave(me);   break;
        case ST_VERIFY: storeRunVerify(me); break;
        case ST_SCAN:   storeRunScan(me);   break;
        default:                            break;
    }
}

/*--------------------------------------------------------------------------*/
/* 状态处理函数                                                              */
/*--------------------------------------------------------------------------*/
static QState initial(Store * const me, void const * const par) {
    (void)par;
    return Q_TRAN(&active);
}

/*..........................................................................*/
static QState active(Store * const me, QEvt const * const e) {
    switch (e->sig) {
        case Q_ENTRY_SIG:
            QTimeEvt_armX(&me->tick, STORE_TICKS, STORE_TICKS);
            return Q_HANDLED();

        /* 三条推进源：周期节拍（写周期等待）、I2C 完成（总线搬完）、
        * 新请求（用户改了参数或要扫描）。它们都只做「往前推一步」。 */
        case STORE_TICK_SIG:
        case STORE_BUS_SIG:
        case STORE_CMD_SIG:
            storeProcess(me);
            return Q_HANDLED();

        default:
            return Q_SUPER(&QHsm_top);
    }
}

/*--------------------------------------------------------------------------*/
/* 中断上下文接口                                                            */
/*--------------------------------------------------------------------------*/
void Store_i2cSink(void) {
    static QEvt const evt = { (QSignal)STORE_BUS_SIG, 0U, 0U };

    (void)QACTIVE_POST_X(AO_Store, &evt, 0U, 0U);
}

/*--------------------------------------------------------------------------*/
/* 给其它 AO / 命令处理用的接口                                              */
/*--------------------------------------------------------------------------*/
char const *App_Store_Value(uint8_t id) {
    return SVC_Param_Value(&l_store.param, id);
}

/*..........................................................................*/
bool App_Store_ConfigResolved(void) {
    return l_store.resolved;
}

/*..........................................................................*/
bool App_Store_IsIdle(void) {
    Store const *me = &l_store;

    /* 快路径：只要还有没落盘的改动，就一定不空闲（VERIFY 通过后才会清 dirty，
     * 所以这一条已经蕴含"不在写"）。 */
    if (me->param.dirty) {
        return false;
    }
    /* 存储关闭：没有可丢的东西，永远空闲。 */
    if (me->phase == ST_DISABLED) {
        return true;
    }
    /* 其余相位里只有 ST_READY 是真正的"等活干"；PROBE/LOAD 是上电加载窗口，
     * SAVE/VERIFY/SCAN 是正在用总线——都不允许复位打断。 */
    return (me->resolved && (me->phase == ST_READY));
}

/*..........................................................................*/
svc_status_t App_Store_SetName(char const *name, char const *value) {
    Store *me = &l_store;
    int16_t id;
    svc_status_t st;

    if (!me->resolved) {
        /* NOTE (为什么拒绝)：启动加载还没结束，此时改的影子会被随后的介质读取
        * 覆盖，出现「回复 ok 但值没变」的假成功。宁可让调用方稍后重试。 */
        return SVC_ERR_STATE;
    }

    id = SVC_Param_Find(name);
    if (id < 0) {
        return SVC_ERR_PARAM;
    }
    st = SVC_Param_SetValue(&me->param, (uint8_t)id, value);
    if (st != SVC_OK) {
        return st;
    }

    if (me->phase != ST_DISABLED) {
        /* 存储关闭时只改内存（置 dirty 也没人会去落盘，留着反而让 AT+CFG? 显示脏） */
        ++me->editSeq;
        me->param.dirty = true;
        storePostCmd();
    }
    return SVC_OK;
}

/*..........................................................................*/
bool App_Store_ResetDefaults(void) {
    Store *me = &l_store;
    uint16_t const ver = me->param.config_version;
    bool const fromMedia = me->param.from_media;

    if (!me->resolved) {
        return false;   /* 启动加载还没结束：此时改动会被随后的读取覆盖 */
    }

    SVC_Param_SetDefaults(&me->param);
    /* NOTE (为什么保留版本号)：config_version 是「配置改过几次」的单调计数，
    * 云端据此核对下发是否生效；恢复默认值也是一次变更，不能把它倒回 0。 */
    me->param.config_version = ver;
    me->param.from_media = fromMedia;
    ++me->editSeq;
    me->param.dirty = true;
    storePostCmd();
    return true;
}

/*..........................................................................*/
bool App_Store_RequestScan(void) {
    Store *me = &l_store;

    if (!me->resolved || (me->phase == ST_DISABLED) || me->scanReq) {
        return false;
    }
    me->scanReq = true;
    storePostCmd();
    return true;
}

/*..........................................................................*/
void App_Store_Info(AppStoreInfo *out) {
    Store const *me = &l_store;

    if (out == (AppStoreInfo *)0) {
        return;
    }
    out->present = me->present;
    out->resolved = me->resolved;
    out->loaded = me->param.from_media;
    out->dirty = me->param.dirty;
    out->slot_seq = me->param.slot_seq;
    out->config_version = me->param.config_version;
    out->saved = me->saved;
    out->failed = me->failed;
}

/*--------------------------------------------------------------------------*/
/* 生命周期                                                                  */
/*--------------------------------------------------------------------------*/
void Store_ctor(void) {
    Store *me = &l_store;

    QActive_ctor(&me->super, Q_STATE_CAST(&initial));
    QTimeEvt_ctorX(&me->tick, &me->super, STORE_TICK_SIG, 0U);

    /* 影子先落编译期默认值：任何时刻 App_Store_Value() 都必须返回合法字符串，
    * 因为 Ble_ctor() 会在介质读回来之前就用它填 BleConfig。 */
    (void)memset(&me->param, 0, sizeof(me->param));
    SVC_Param_SetDefaults(&me->param);

    DRV_AT24C04_Init();

    me->sub = STORE_SLOT_A;
    me->attempts = 0U;
    me->waiting = false;
    me->present = false;
    me->scanReq = false;
    me->curSlot = STORE_SLOT_NONE;
    me->slotOk[STORE_SLOT_A] = false;
    me->slotOk[STORE_SLOT_B] = false;
    me->readAddr = 0U;
    me->readLen = 0U;
    me->writeSlot = STORE_SLOT_A;
    me->writeAddr = 0U;
    me->writeSeq = 0U;
    me->writeLen = 0U;
    me->nextVer = 0U;
    me->scanAddr = 0U;
    me->scanCount = 0U;
    me->saved = 0U;
    me->failed = 0U;
    me->editSeq = 0U;
    me->saveEditSeq = 0U;

#if (APP_CFG_STORE_ENABLE == 0)
    /* 功能关闭：不碰总线，也不让 AO_Ble 等配置就绪（resolved 立刻为真）。 */
    me->phase = ST_DISABLED;
    me->resolved = true;
#else
    me->phase = ST_PROBE;
    me->resolved = false;
#endif
}

/*..........................................................................*/
void Store_start(void) {
    QACTIVE_START(
        AO_Store,
        APP_STORE_PRIO,         /* 优先级来自 App 层装配表 */
        l_queue,                /* 队列存储属于 AO 自己     */
        Q_DIM(l_queue),
        (void *)0, 0U,          /* QV 内核不需要私有栈      */
        (QEvt *)0               /* 无初始化事件             */
    );
}
