/*============================================================================
* sensor.h —— 应用层：AO_Sensor（SCL3300 三轴倾角采集 / 滤波 / 组帧上报）
*
* 层次定位：**App(应用层)**，依赖 Service 与 Driver，不直接接触 HAL。
*
* 数据来源：SCL3300-D01 三轴数字倾角仪（SPI，片选 PA4），测量模式 **Mode 3**
*   （手册：±10°、10 Hz 一阶低通、噪声密度 32 µg）。
*
* 职责：按 1 Hz 周期发起一次采样，取回「三轴倾角 + 三轴加速度 + 片内温度」，
*       先做滑动平均平滑，再用 Service 层组一帧二进制数据并立即解析回来做自检
*       （CRC 校验），最后以 ASCII 行输出到调试串口。
*
* 状态机：Idle（等待启动） ⇄ Sampling（周期采样）
* 接收的跨 AO 事件（全局段）：
*   SYS_SENSOR_START_SIG   开始周期采样（由装配点 App_start 投递）
*   SYS_SENSOR_STOP_SIG    停止周期采样
*   SYS_SENSOR_REPORT_SIG  立即上报一次（不改变状态）
*
* NOTE (2026-10-10 换掉仿真源)：本文件原先描述「温度 + 湿度」，数据来自
*   drv_sensor.c 的仿真实现。接入真实 SCL3300 后：仿真实现已删除，
*   **湿度通道被移除**（该器件没有湿度），改为三轴倾角 + 加速度 + 片内温度。
*===========================================================================*/
#ifndef SENSOR_H
#define SENSOR_H

#include "qpc.h"
#include "svc_filter.h"
#include "drv_scl3300.h"

/*! 上报帧的命令码。
*  载荷布局（小端）：X/Y/Z 倾角各 int16（0.01°）6 B + 温度 int16（0.01 °C）2 B
*  + 三轴加速度各 int16（mg）6 B + STATUS uint16 2 B = 16 B。 */
#define SENSOR_CMD_REPORT (0x10U)

/*! 上报载荷长度（字节）。 */
#define SENSOR_REPORT_PAYLOAD_LEN (16U)

typedef struct {
    QActive  super;             /* 基类，必须放在首位                       */
    QTimeEvt sampleEvt;         /* 私有时间事件：1 Hz 采样                  */
    QTimeEvt processEvt;        /* 私有时间事件：推进驱动的非阻塞状态机      */

    /* 滤波状态（状态由 AO 自己持有，Service 层不定义全局量） */
    SvcMavg  angFilt[DRV_SENSOR_AXIS_COUNT];

    uint32_t seq;               /* 采样次数                                 */
    uint32_t frames;            /* 组帧 + 自检成功的次数                    */
    uint32_t errors;            /* 采样失败次数                             */

    int32_t  angLast_cdeg[DRV_SENSOR_AXIS_COUNT]; /* 最近一次三轴倾角，0.01° */
    int32_t  tempLast_cC;       /* 最近一次片内温度，0.01 °C                */
    int32_t  accLast_mg[DRV_SENSOR_AXIS_COUNT];   /* 最近一次三轴加速度，mg   */
    uint16_t statusLast;        /* 最近一次读到的 STATUS                    */
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
