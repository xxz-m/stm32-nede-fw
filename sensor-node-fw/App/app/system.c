/*============================================================================
* system.c —— AO_System：延迟复位状态机
* 层次定位：App 层；唯一允许调用 `BSP_systemReset()` 的业务代码（BSP 只做搬运）。
*
* 状态与流程（单状态 Active，靠 100 ms 时间事件推进）：
*
*   App_System_RequestReboot(src)
*          │ 投 SYS_REBOOT_REQUEST_SIG（只投事件，不改状态）
*          ▼
*   [请求] ──SYSTEM_TICK_SIG──> 最短延时到？ ──否──> 继续等
*          │                          │是
*          │                          ▼
*          │             硬上限到？ ──否──> 落盘干净 && 上行空闲？ ──否──> 继续等
*          │                          │是                    │是
*          └──────────────────────────┴──────────────────────┴──> 打日志 → BSP_logFlush() → BSP_systemReset()
*
* NOTE (为什么用"硬上限"而不是纯条件等待)：见 app_config.h 里
*   `APP_CFG_SYSTEM_REBOOT_MAX_WAIT_MS` 的 NOTE——条件永远等不到时，设备必须还能重启。
* NOTE (为什么先 flush 再复位)：复位会丢掉 USART1 发送环里还没搬出去的字节，而
*   「谁请求的、等了多久」正是下次启动排障时要看的那一行。`BSP_logFlush()` 是有界忙等、
*   靠中断搬队列，不依赖 QV 调度，所以复位前调用是安全的。
* NOTE (不要在这里等 EEPROM)：落盘要靠 AO_Store 跑起来，忙等会把它锁死——
*   所以本文件一律用「时间事件 + 只读判断」，绝不循环等待。
*===========================================================================*/
#include "system.h"
#include "app_config.h"
#include "app_signals.h"
#include "app_prio.h"
#include "bsp.h"
#include "store.h"
#include "svc_log.h"
#if (APP_CFG_MODEM_ENABLE != 0U)
#include "modem.h"
#endif

#include <stdbool.h>

Q_DEFINE_THIS_MODULE("system")
void Q_onAssert(char const * const module, int loc);

/*! 倒计时检查周期换算成 QP 节拍（至少 1 拍：算出 0 会变成每拍都跑）。 */
#define SYSTEM_TICKS \
    (((APP_CFG_SYSTEM_PROCESS_PERIOD_MS * BSP_TICKS_PER_SEC) / 1000U) > 0U \
         ? ((APP_CFG_SYSTEM_PROCESS_PERIOD_MS * BSP_TICKS_PER_SEC) / 1000U) \
         : 1U)

typedef struct {
    QActive super;              /*!< 基类，必须放在首位            */
    QTimeEvt tick;              /*!< 100 ms 倒计时节拍             */
    bool requested;             /*!< 已收到重启请求，正在倒计时    */
    bool logged;                /*!< 已打过"请求重启"那行日志      */
    AppRebootSource source;     /*!< 请求来源（仅日志）            */
    uint32_t requestMs;         /*!< 收到请求的时刻（BSP_uptimeMs）*/
} System;

static System l_system;
QActive * const AO_System = &l_system.super;
static QEvt const *l_queue[4];
static QState active(System * const me, QEvt const * const e);

/*! 待投递的重启来源。只在「App_System_RequestReboot() 投递前写入」与
*  「SYS_REBOOT_REQUEST_SIG 处理时读出」这一对位置使用：事件本身不带载荷，
*  而 QV 协作式、投递同步，中间不存在被抢占的窗口。 */
static AppRebootSource l_pendingSource = APP_REBOOT_SOURCE_SERIAL;

/** @brief 来源转短名（ASCII，见 AGENTS.md 5.2）。 */
static char const *sourceName(AppRebootSource source) {
    switch (source) {
        case APP_REBOOT_SOURCE_SERIAL: return "serial";
        case APP_REBOOT_SOURCE_CLOUD:  return "cloud";
        case APP_REBOOT_SOURCE_BLE:    return "ble";
        /* 身份参数（SN）改完自动重启：日志里必须能一眼看出"不是谁发的命令、
         * 而是固件自己决定重启"，否则现场会以为是设备无故复位。 */
        case APP_REBOOT_SOURCE_IDENTITY: return "identity";
        default:                       return "unknown";
    }
}

/** @brief 上行链路是否已经没有待发的东西（4G 关闭时恒为空闲）。
*
* NOTE (只看两个字段)：`reply_pending` = 我们自己排队但还没发出去的命令应答；
*   `runtime_op != NONE` = 有一条上行命令正在等模组应答。两者都空，说明"该发的都发出去了"。
*   模组没在线（重连中）时这两个字段本来就是空，复位不受影响——这也正是我们要的：
*   连不上网时 `AT+REBOOT` 不该被链路状态拖住。
*/
static bool uplinkIdle(void) {
#if (APP_CFG_MODEM_ENABLE != 0U)
    return Modem_IsUplinkIdle();
#else
    return true;
#endif
}

/** @brief 真正复位：先留证据，再排空串口，最后复位。 */
static void resetNow(System * const me) {
    /* NOTE (为什么先 flush 一次再写日志)：发送环满时 `LOG_*` 会丢弃，而这一行正是
     *   下次启动时要看的证据。先把已经排队的输出（命令回复、前面的日志）搬出去腾出空间，
     *   再写自己这一行，最后再排空一次——两次都是有界忙等，且紧接着就复位。 */
    BSP_logFlush();
    LOG_W("system: rebooting now, src=%s waited=%lu ms",
          sourceName(me->source),
          (unsigned long)(BSP_uptimeMs() - me->requestMs));
    BSP_logFlush();
    BSP_systemReset();
}

static QState initial(System * const me, void const * const par) {
    (void)par;
    return Q_TRAN(&active);
}

static QState active(System * const me, QEvt const * const e) {
    switch (e->sig) {
        case Q_ENTRY_SIG:
            QTimeEvt_armX(&me->tick, SYSTEM_TICKS, SYSTEM_TICKS);
            return Q_HANDLED();

        /* 收到重启请求：只记时间与来源，真正的复位在 SYSTEM_TICK_SIG 里做。
         * NOTE (幂等)：已经在倒计时时忽略后续请求，**不重置计时**——否则平台重发
         *   或串口连敲会把重启一直往后推，最坏情况永远不重启。 */
        case SYS_REBOOT_REQUEST_SIG:
            if (me->requested) {
                LOG_W("system: reboot already pending, ignoring");
                return Q_HANDLED();
            }
            me->requested = true;
            me->logged = false;
            me->source = l_pendingSource;   /* 由 App_System_RequestReboot 在投递前写入 */
            me->requestMs = BSP_uptimeMs();
            return Q_HANDLED();

        case SYSTEM_TICK_SIG: {
            uint32_t elapsed;

            if (!me->requested) {
                return Q_HANDLED();
            }
            elapsed = BSP_uptimeMs() - me->requestMs;
            if (!me->logged) {
                me->logged = true;
                LOG_W("system: reboot requested by %s, waiting for reply+store",
                      sourceName(me->source));
            }
            /* 1) 最短延时：保证回复有机会出设备 */
            if (elapsed < (uint32_t)APP_CFG_SYSTEM_REBOOT_MIN_DELAY_MS) {
                return Q_HANDLED();
            }
            /* 2) 硬上限之内才看条件；到点无条件复位（见 app_config.h 的 NOTE） */
            if (elapsed < (uint32_t)APP_CFG_SYSTEM_REBOOT_MAX_WAIT_MS) {
                if (!App_Store_IsIdle()) {
                    return Q_HANDLED();
                }
                if (!uplinkIdle()) {
                    return Q_HANDLED();
                }
            }
            resetNow(me);
            return Q_HANDLED();
        }

        default:
            return Q_SUPER(&QHsm_top);
    }
}

/*--------------------------------------------------------------------------*/
/* 对外入口                                                                  */
/*--------------------------------------------------------------------------*/
void App_System_RequestReboot(AppRebootSource source) {
    static QEvt const evt = { (QSignal)SYS_REBOOT_REQUEST_SIG, 0U, 0U };

    /* NOTE (为什么用模块静态量传来源)：事件本身不带载荷（QEvt 无字段），而来源只用于
     *   一行日志。QV 是协作式的：本函数执行完之前 AO_System 不可能被调度，投递又是
     *   同步完成，因此这个赋值与事件的送达之间不存在竞争。 */
    l_pendingSource = source;
    (void)QACTIVE_POST_X(AO_System, &evt, 0U, 0U);
}

/*--------------------------------------------------------------------------*/
void System_ctor(void) {
    System *me = &l_system;

    QActive_ctor(&me->super, Q_STATE_CAST(&initial));
    QTimeEvt_ctorX(&me->tick, &me->super, SYSTEM_TICK_SIG, 0U);
    me->requested = false;
    me->logged = false;
    me->source = APP_REBOOT_SOURCE_SERIAL;
    me->requestMs = 0U;
}

void System_start(void) {
    QACTIVE_START(AO_System, APP_SYSTEM_PRIO, l_queue, Q_DIM(l_queue),
                  (void *)0, 0U, (QEvt *)0);
}
