/*============================================================================
* heartbeat.h —— 应用层：AO_Heartbeat（每 2 秒打印一次心跳）
*
* 层次定位：**App(应用层)** → Service → Driver → BSP → HAL/CMSIS
*
* 职责：用私有时间事件每 APP_CFG_HEARTBEAT_PERIOD_S 秒（默认 2 s）经**异步
*       日志**输出一行心跳，演示「Service 日志 + 时间事件」的最小组合。
*
* 状态机：只有 Running 一个状态（不需要启动/停止的双态；需要时再加）。
* 信号：使用 Heartbeat 段（见 App/app/app_signals.h）。
*===========================================================================*/
#ifndef HEARTBEAT_H
#define HEARTBEAT_H

#include "qpc.h"

typedef struct {
    QActive  super;     /* 基类，必须放在首位              */
    QTimeEvt timeEvt;   /* 私有时间事件：心跳周期          */
    uint32_t seq;       /* 心跳计数                        */
    uint32_t dropped;   /* 日志队列满导致的丢弃计数（诊断用） */
} Heartbeat;

/* 状态处理函数 */
QState Heartbeat_initial(Heartbeat * const me, void const * const par);
QState Heartbeat_state_Running(Heartbeat * const me, QEvt const * const e);

/*! 构造 AO（只做对象初始化，不启动）。 */
void Heartbeat_ctor(void);

/*! 启动 AO：事件队列存储由本模块自己持有并交给 QP 内核。 */
void Heartbeat_start(void);

/*! 单例句柄。 */
extern QActive * const AO_Heartbeat;

#endif /* HEARTBEAT_H */
