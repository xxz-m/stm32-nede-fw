/*============================================================================
* heartbeat.c —— 应用层：AO_Heartbeat 实现
*
* 层次定位：App 层。链路：时间事件 → Service 日志（异步队列）→ BSP 发送中断。
*
* 周期来自 APP_CFG_HEARTBEAT_PERIOD_S（默认 2 s），换算成 QP 节拍：
*   APP_CFG_HEARTBEAT_PERIOD_S * BSP_TICKS_PER_SEC
*===========================================================================*/
#include "heartbeat.h"
#include "app_signals.h"
#include "app_prio.h"
#include "app_config.h"
#include "svc_log.h"
#include "bsp.h"

/* 本模块在 QP 断言里的名字 */
#if (APP_CFG_HEARTBEAT_ENABLE != 0)
Q_DEFINE_THIS_MODULE("heartbeat")
#endif

/* QP/C 断言钩子（实现在 App/bsp/bsp.c） */
void Q_onAssert(char const * const module, int loc);

/* AO 实例与事件队列（队列存储归 AO 自己所有） */
static Heartbeat l_heartbeat;
QActive * const AO_Heartbeat = &l_heartbeat.super;
#if (APP_CFG_HEARTBEAT_ENABLE != 0)
static QEvt const *l_heartbeatQueueSto[4];
#endif

/* 心跳周期（QP 节拍数） */
#define HEARTBEAT_TICKS ((QTimeEvtCtr)(APP_CFG_HEARTBEAT_PERIOD_S * BSP_TICKS_PER_SEC))

/*--------------------------------------------------------------------------*/
QState Heartbeat_state_Running(Heartbeat * const me, QEvt const * const e) {
    QState status;

    switch (e->sig) {
        case Q_ENTRY_SIG: {
            me->seq = 0U;
            QTimeEvt_armX(&me->timeEvt, HEARTBEAT_TICKS, HEARTBEAT_TICKS);
            LOG_I("heartbeat: started, period %u s", (unsigned)APP_CFG_HEARTBEAT_PERIOD_S);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            QTimeEvt_disarm(&me->timeEvt);
            status = Q_HANDLED();
            break;
        }
        case HEARTBEAT_TICK_SIG: {
            ++me->seq;
            /* 日志是异步的：这里只格式化并入队，不等串口发完 */
            LOG_I("heartbeat: alive, t=%u s",
                  (unsigned)(me->seq * (uint32_t)APP_CFG_HEARTBEAT_PERIOD_S));
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/*..........................................................................*/
QState Heartbeat_initial(Heartbeat * const me, void const * const par) {
    (void)me;
    (void)par;

    return Q_TRAN(&Heartbeat_state_Running);
}

/*..........................................................................*/
void Heartbeat_ctor(void) {
    Heartbeat *me = (Heartbeat *)AO_Heartbeat;

    QActive_ctor(&me->super, Q_STATE_CAST(&Heartbeat_initial));
    QTimeEvt_ctorX(&me->timeEvt, &me->super, HEARTBEAT_TICK_SIG, 0U);
    me->seq = 0U;
    me->dropped = 0U;
}

/*..........................................................................*/
void Heartbeat_start(void) {
#if (APP_CFG_HEARTBEAT_ENABLE != 0)
    QACTIVE_START(
        AO_Heartbeat,
        APP_HEARTBEAT_PRIO,
        l_heartbeatQueueSto,
        Q_DIM(l_heartbeatQueueSto),
        (void *)0, 0U,
        (QEvt *)0
    );
#endif
}
