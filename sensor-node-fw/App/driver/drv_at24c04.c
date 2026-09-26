/*============================================================================
* drv_at24c04.c —— 器件驱动层：AT24C04 非阻塞实现
*
* 层次定位：Driver 层。只调用 BSP_i2c_*，不碰 HAL、不含 QP。
*
* 状态机（Phase）：
*
*   IDLE ──Start()──> ISSUE ──启动一次IT传输──> WAIT ──完成──┬─> DONE（读完/写完）
*                     ^                                     ├─> HOLD（写：等 tWR）
*                     └──────────── HOLD 到期 ──────────────┘
*                     任何一段失败 ──> 退避一个节拍重试（共 RETRY 次）
*                                  ──> 补时钟恢复总线，再给一次机会
*                                  ──> FAILED
*
* 时间参数全部取自 app_config.h，不写裸数字（见 AGENTS.md 第 6 节）。
*===========================================================================*/
#include "drv_at24c04.h"
#include "app_config.h"
#include "bsp.h"

/*--------------------------------------------------------------------------*/
/* 内部类型与状态                                                            */
/*--------------------------------------------------------------------------*/
typedef enum {
    JOB_NONE = 0,
    JOB_PROBE,      /*!< 读 1 字节探测 ACK           */
    JOB_READ,       /*!< 连续读                      */
    JOB_WRITE       /*!< 连续写（按页拆分）          */
} drvJob_t;

typedef enum {
    PH_IDLE = 0,    /*!< 空闲，可以接受新作业        */
    PH_ISSUE,       /*!< 该发下一段总线传输了        */
    PH_WAIT,        /*!< 等中断传输结束              */
    PH_HOLD,        /*!< 写周期等待（tWR）           */
    PH_DONE,        /*!< 作业成功结束                */
    PH_FAIL         /*!< 作业失败结束                */
} drvPhase_t;

/*! 「一直拿不到总线」的容忍次数（单位：Process 调用次数）。
* NOTE (为什么要这个上限)：AO 的推进不能被任何外设拖死。若外设状态异常导致
* 每拍都返回 BUSY，这里到点后把它升级成一次失败（进而走重试/总线恢复/放弃），
* 而不是让状态机永远停在同一拍。 */
#define DRV_AT24C04_BUSY_STALL_LIMIT (20U)

static drvJob_t     l_job;
static drvPhase_t   l_phase;
static uint16_t     l_addr;          /* 器件内起始地址                */
static uint16_t     l_len;           /* 作业总字节数                  */
static uint16_t     l_pos;           /* 本作业已完成的字节数          */
static uint16_t     l_chunk;         /* 本段传输的字节数              */
static uint8_t     *l_rbuf;          /* 读目标                        */
static uint8_t const *l_wbuf;        /* 写数据源                      */
static uint8_t      l_probeByte;     /* 探测读到的 1 字节（丢弃）     */
static uint32_t     l_deadline;      /* PH_WAIT / PH_HOLD 的期限      */
static drv_status_t l_result;
static uint8_t      l_retry;         /* 本作业已用掉的重试次数        */
static bool         l_recovered;     /* 本作业是否已做过总线恢复       */
static uint8_t      l_stall;         /* 连续「拿不到总线」的拍数       */

/*--------------------------------------------------------------------------*/
/* 内部工具                                                                  */
/*--------------------------------------------------------------------------*/
/** @brief 把器件内地址翻成 8 位从机地址字节。
*
* AT24C04 的第 9 位地址由器件地址的 P0 位承载：0x000..0x0FF → 0xA0，0x100..0x1FF → 0xA2
* （7 位地址 0x50 左移后为 0xA0，P0 落在 bit1）。换 A1/A2 接线只改 APP_CFG_STORE_I2C_ADDR。
*/
static uint8_t devAddrOf(uint16_t addr) {
    uint8_t const block = (uint8_t)((addr >> 8) & 0x01U);

    return (uint8_t)(((uint8_t)(APP_CFG_STORE_I2C_ADDR << 1)) | (uint8_t)(block << 1));
}

/** @brief 毫秒期限是否已到（用有符号差值，天然处理 32 位回绕）。 */
static bool deadlinePassed(uint32_t now, uint32_t deadline) {
    return ((int32_t)(now - deadline) >= 0);
}

/** @brief BSP 的结束状态翻成 Driver 层状态码。 */
static drv_status_t mapBusStatus(bsp_status_t st) {
    return (st == BSP_ERR_TIMEOUT) ? DRV_ERR_TIMEOUT : DRV_ERR_BUS;
}

/** @brief 判定某个作业能否被接受。 */
static drv_status_t jobAcceptable(drvJob_t job, uint8_t const *wbuf, uint8_t *rbuf,
                                  uint16_t addr, uint16_t len) {
    if ((l_phase == PH_ISSUE) || (l_phase == PH_WAIT) || (l_phase == PH_HOLD)) {
        return DRV_ERR_BUSY;    /* 上一个作业还没结束 */
    }
    if (job == JOB_PROBE) {
        return DRV_OK;          /* 探测不需要缓冲 */
    }
    if ((len == 0U) ||
        ((uint32_t)addr + (uint32_t)len > (uint32_t)APP_CFG_STORE_CAPACITY)) {
        return DRV_ERR_PARAM;
    }
    if ((job == JOB_WRITE) && (wbuf == (uint8_t const *)0)) {
        return DRV_ERR_PARAM;
    }
    if ((job == JOB_READ) && (rbuf == (uint8_t *)0)) {
        return DRV_ERR_PARAM;
    }
    return DRV_OK;
}

/** @brief 装载一个新作业并进入 PH_ISSUE。 */
static void jobLoad(drvJob_t job, uint16_t addr, uint8_t *rbuf,
                    uint8_t const *wbuf, uint16_t len) {
    l_job = job;
    l_addr = addr;
    l_len = len;
    l_pos = 0U;
    l_chunk = 0U;
    l_rbuf = rbuf;
    l_wbuf = wbuf;
    l_retry = 0U;
    l_recovered = false;
    l_stall = 0U;
    l_result = DRV_OK;
    l_phase = PH_ISSUE;
}

/** @brief 一段传输失败：先退避重试，再恢复总线，最后才判失败。
*
* NOTE (为什么退避一个节拍)：Process() 由 AO 周期调用，这里直接回到 PH_ISSUE
* 意味着「下一次 Process 再发」，天然获得一个节拍的退避，不需要额外的延时状态。
*/
static void jobFailed(drv_status_t err) {
    if (l_job == JOB_PROBE) {
        err = DRV_ERR_NODEV;    /* 探测读不回来 = 器件不应答，不是总线问题 */
    }

    if (l_retry < APP_CFG_STORE_RETRY) {
        ++l_retry;
        l_phase = PH_ISSUE;
        return;
    }
    if (!l_recovered) {
        /* 重试用尽：最后手段是补时钟把总线从从机手里抢回来，再给一次机会。
        * 这正是「EEPROM 明明在、却一直超时」的典型现场（上次复位卡在半途）。 */
        l_recovered = true;
        l_retry = 0U;
        (void)BSP_i2cBusRecover();
        l_phase = PH_ISSUE;
        return;
    }

    l_result = err;
    l_phase = PH_FAIL;
}

/*--------------------------------------------------------------------------*/
/* 对外：容量与页大小                                                        */
/*--------------------------------------------------------------------------*/
uint16_t DRV_AT24C04_Capacity(void) {
    return (uint16_t)APP_CFG_STORE_CAPACITY;
}

/*..........................................................................*/
uint16_t DRV_AT24C04_PageSize(void) {
    return (uint16_t)APP_CFG_STORE_PAGE_SIZE;
}

/*--------------------------------------------------------------------------*/
/* 对外：初始化                                                              */
/*--------------------------------------------------------------------------*/
void DRV_AT24C04_Init(void) {
    l_job = JOB_NONE;
    l_phase = PH_IDLE;
    l_addr = 0U;
    l_len = 0U;
    l_pos = 0U;
    l_chunk = 0U;
    l_rbuf = (uint8_t *)0;
    l_wbuf = (uint8_t const *)0;
    l_deadline = 0U;
    l_result = DRV_OK;
    l_retry = 0U;
    l_recovered = false;
    l_stall = 0U;
}

/*--------------------------------------------------------------------------*/
/* 对外：发起作业                                                            */
/*--------------------------------------------------------------------------*/
drv_status_t DRV_AT24C04_ProbeStart(void) {
    drv_status_t st = jobAcceptable(JOB_PROBE, (uint8_t const *)0, (uint8_t *)0, 0U, 0U);

    if (st != DRV_OK) {
        return st;
    }
    jobLoad(JOB_PROBE, 0U, &l_probeByte, (uint8_t const *)0, 1U);
    return DRV_OK;
}

/*..........................................................................*/
drv_status_t DRV_AT24C04_ReadStart(uint16_t addr, uint8_t *buf, uint16_t len) {
    drv_status_t st = jobAcceptable(JOB_READ, (uint8_t const *)0, buf, addr, len);

    if (st != DRV_OK) {
        return st;
    }
    jobLoad(JOB_READ, addr, buf, (uint8_t const *)0, len);
    return DRV_OK;
}

/*..........................................................................*/
drv_status_t DRV_AT24C04_WriteStart(uint16_t addr, uint8_t const *buf, uint16_t len) {
    drv_status_t st = jobAcceptable(JOB_WRITE, buf, (uint8_t *)0, addr, len);

    if (st != DRV_OK) {
        return st;
    }
    jobLoad(JOB_WRITE, addr, (uint8_t *)0, buf, len);
    return DRV_OK;
}

/*--------------------------------------------------------------------------*/
/* 对外：状态与结果                                                          */
/*--------------------------------------------------------------------------*/
drv_at24c04_state_t DRV_AT24C04_State(void) {
    switch (l_phase) {
        case PH_ISSUE:
        case PH_WAIT:
        case PH_HOLD:
            return DRV_AT24C04_STATE_BUSY;
        case PH_DONE:
            return DRV_AT24C04_STATE_DONE;
        case PH_FAIL:
            return DRV_AT24C04_STATE_FAILED;
        default:
            return DRV_AT24C04_STATE_IDLE;
    }
}

/*..........................................................................*/
drv_status_t DRV_AT24C04_Result(void) {
    return l_result;
}

/*..........................................................................*/
char const *DRV_AT24C04_ErrStr(drv_status_t st) {
    switch (st) {
        case DRV_OK:          return "ok";
        case DRV_ERR_PARAM:   return "param";
        case DRV_ERR_BUS:     return "bus";
        case DRV_ERR_TIMEOUT: return "timeout";
        case DRV_ERR_NODEV:   return "nodev";
        case DRV_ERR_BUSY:    return "busy";
        default:              return "?";
    }
}

/*--------------------------------------------------------------------------*/
/* 状态机推进                                                                */
/*--------------------------------------------------------------------------*/
/** @brief PH_ISSUE：按页与 256 B 块边界切一段并启动中断传输。 */
static void phaseIssue(uint32_t now) {
    uint16_t at;
    uint16_t left;
    uint16_t pageLeft;
    uint16_t blockLeft;
    uint8_t dev;
    bsp_status_t bst;

    if (BSP_i2cIsBusy()) {
        /* 正常路径下 PH_ISSUE 时总线一定是空的；走到这里说明外设状态异常，
        * 计数到上限就升级成失败，避免状态机永远停在这一拍。 */
        if (++l_stall > (uint8_t)DRV_AT24C04_BUSY_STALL_LIMIT) {
            l_stall = 0U;
            jobFailed(DRV_ERR_TIMEOUT);
        }
        return;
    }

    if (l_job == JOB_PROBE) {
        l_chunk = 1U;
        dev = devAddrOf(l_addr);
        bst = BSP_i2cMemReadIt(dev, (uint8_t)(l_addr & 0xFFU), &l_probeByte, 1U);
        if (bst == BSP_ERR_BUSY) {
            if (++l_stall > (uint8_t)DRV_AT24C04_BUSY_STALL_LIMIT) {
                l_stall = 0U;
                jobFailed(DRV_ERR_TIMEOUT);
            }
            return;
        }
        if (bst != BSP_OK) {
            jobFailed(mapBusStatus(bst));
            return;
        }
        l_stall = 0U;
        l_phase = PH_WAIT;
        l_deadline = now + APP_CFG_STORE_XFER_TIMEOUT_MS;
        return;
    }

    at = (uint16_t)(l_addr + l_pos);
    left = (uint16_t)(l_len - l_pos);

    /* NOTE (两处边界)：① 不能跨 16 B 页——AT24C04 跨页写会回卷覆盖页首；
    * ② 不能跨 256 B 块——第 9 位地址由器件地址携带，跨块必须重发地址。 */
    pageLeft = (uint16_t)(APP_CFG_STORE_PAGE_SIZE - (at % APP_CFG_STORE_PAGE_SIZE));
    blockLeft = (uint16_t)(256U - (at & 0xFFU));
    l_chunk = left;
    if (l_chunk > pageLeft) {
        l_chunk = pageLeft;
    }
    if (l_chunk > blockLeft) {
        l_chunk = blockLeft;
    }

    dev = devAddrOf(at);
    if (l_job == JOB_WRITE) {
        bst = BSP_i2cMemWriteIt(dev, (uint8_t)(at & 0xFFU), &l_wbuf[l_pos], l_chunk);
    }
    else {
        bst = BSP_i2cMemReadIt(dev, (uint8_t)(at & 0xFFU), &l_rbuf[l_pos], l_chunk);
    }

    if (bst == BSP_ERR_BUSY) {
        if (++l_stall > (uint8_t)DRV_AT24C04_BUSY_STALL_LIMIT) {
            l_stall = 0U;
            jobFailed(DRV_ERR_TIMEOUT);
        }
        return;
    }
    if (bst != BSP_OK) {
        jobFailed(mapBusStatus(bst));
        return;
    }

    l_stall = 0U;
    l_phase = PH_WAIT;
    l_deadline = now + APP_CFG_STORE_XFER_TIMEOUT_MS;
}

/*..........................................................................*/
/** @brief PH_WAIT：等中断传输结束，成功后决定下一步。 */
static void phaseWait(uint32_t now) {
    if (BSP_i2cIsBusy()) {
        if (deadlinePassed(now, l_deadline)) {
            jobFailed(DRV_ERR_TIMEOUT);   /* 回调一直不来：外设或中断出了问题 */
        }
        return;
    }

    if (BSP_i2cLastStatus() != BSP_OK) {
        jobFailed(mapBusStatus(BSP_i2cLastStatus()));
        return;
    }

    l_pos = (uint16_t)(l_pos + l_chunk);

    if (l_job == JOB_PROBE) {
        l_phase = PH_DONE;
        return;
    }
    if (l_pos >= l_len) {
        l_phase = PH_DONE;
        return;
    }
    if (l_job == JOB_WRITE) {
        /* 本页写进去了，等 EEPROM 内部写周期结束才能碰下一段 */
        l_phase = PH_HOLD;
        l_deadline = now + APP_CFG_STORE_WRITE_CYCLE_MS;
        return;
    }
    l_phase = PH_ISSUE;         /* 读：直接接着读下一块 */
}

/*..........................................................................*/
void DRV_AT24C04_Process(void) {
    uint32_t const now = BSP_uptimeMs();

    switch (l_phase) {
        case PH_ISSUE:
            phaseIssue(now);
            break;
        case PH_WAIT:
            phaseWait(now);
            break;
        case PH_HOLD:
            if (deadlinePassed(now, l_deadline)) {
                l_phase = PH_ISSUE;
            }
            break;
        default:
            break;              /* IDLE / DONE / FAIL：无事可做 */
    }
}
