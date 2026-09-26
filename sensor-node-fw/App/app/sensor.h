/*============================================================================
* sensor.h —— 应用层：AO_Sensor（周期采样 / 滤波 / 组帧上报）
*
* 层次定位：**App(应用层)**，依赖 Service 与 Driver，不直接接触 HAL。
*
* 职责：按 1 Hz 周期采样温度与湿度，先做中值滤波去尖峰、再做滑动平均，
*       然后用 Service 层组一帧二进制数据并立即解析回来做自检（CRC 校验），
*       最后以 ASCII 行输出到调试串口。
*
* 状态机：Idle（等待启动） ⇄ Sampling（周期采样）
* 接收的跨 AO 事件（全局段）：
*   SYS_SENSOR_START_SIG   开始周期采样（由 AO_Node 的控制台命令转发）
*   SYS_SENSOR_STOP_SIG    停止周期采样
*   SYS_SENSOR_REPORT_SIG  立即上报一次（不改变状态）
*===========================================================================*/
#ifndef SENSOR_H
#define SENSOR_H

#include "qpc.h"
#include "svc_filter.h"

/* 上报帧的命令码（载荷：温度 int32 小端 4 B + 湿度 uint16 小端 2 B） */
#define SENSOR_CMD_REPORT (0x10U)

typedef struct {
    QActive  super;         /* 基类，必须放在首位                     */
    QTimeEvt sampleEvt;     /* 私有时间事件：1 Hz 采样                */
    SvcMavg  tempFilt;      /* 温度滑动平均状态（状态由 AO 自己持有） */
    SvcMavg  rhFilt;        /* 湿度滑动平均状态                       */
    uint32_t seq;           /* 采样次数                               */
    uint32_t frames;        /* 组帧 + 自检成功的次数                  */
    int32_t  tempLast_cC;   /* 最近一次温度（0.01 °C）                */
    uint16_t rhLast_permil; /* 最近一次湿度（0.1 %RH）                */
} Sensor;

/* 状态处理函数 */
QState Sensor_initial(Sensor * const me, void const * const par);
QState Sensor_state_Idle(Sensor * const me, QEvt const * const e);
QState Sensor_state_Sampling(Sensor * const me, QEvt const * const e);

/*! 构造 AO（只做对象初始化，不启动）。 */
void Sensor_ctor(void);

/*! 启动 AO：队列存储由本模块自己持有，并交给 QP 内核。 */
void Sensor_start(void);

/*! 单例句柄。 */
extern QActive * const AO_Sensor;

#endif /* SENSOR_H */
