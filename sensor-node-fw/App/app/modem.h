/*============================================================================
* modem.h —— 应用层 4G 活动对象与采样快照事件
* 层次定位：App 层；以事件驱动可移植 4G 组件，不向组件泄漏 QP。
*===========================================================================*/
#ifndef APP_MODEM_H
#define APP_MODEM_H

#include "qpc.h"
#include <stdint.h>

/*! AO_Sensor 发来的采样快照。
*  NOTE (2026-10-10)：湿度已随仿真源删除（SCL3300 无湿度通道），
*  改为三轴倾角（0.01° 标度）+ 片内温度（0.01 °C）。 */
typedef struct {
    QEvt super;
    int32_t temperature_cC;   /* 片内温度，0.01 °C */
    int32_t angle_x_cdeg;     /* X 轴倾角，0.01°   */
    int32_t angle_y_cdeg;     /* Y 轴倾角，0.01°   */
    int32_t angle_z_cdeg;     /* Z 轴倾角，0.01°   */
    int32_t acc_mg[3];        /* 三轴加速度，毫g，仅用于寄存器读取。 */
    uint16_t battery_mV;      /* 电池电压，毫伏；有效性必须同时检查。 */
    bool battery_valid;      /* false 时不得上传旧值或零值占位。 */
    bool valid;              /* false 为本轮采样失败，不得发送历史值。 */
    bool cloud_requested;    /* 统一采集批次用于云端上报。 */
} ModemMeasurementEvt;

/** @brief 所有来源请求同一采集上报事件；true仅表示入队，忙时合并。 */
bool Modem_RequestReport(void);
void Modem_RequestSignal(void);
void Modem_ctor(void);
void Modem_start(void);
void Modem_uartRxIsrSink(void);
extern QActive * const AO_Modem;

/*! 上行链路是否已经没有待发的东西（只读；给 AO_System 的重启倒计时用）。
*
* @return true = 没有排队的命令应答、也没有正在等模组应答的上行命令。
*
* NOTE (为什么需要它)：`AT+REBOOT` 的回复正是走这条上行链路（`command/reply`）。
*   复位太早，模组还没把 PUBLISH 发出去，平台就会判「指令超时」，而设备已经重启——
*   两边现象对不上。判据只看组件里那两个"有没有活"的字段，不碰总线、不阻塞。
* NOTE (模组不在线时)：连接计划期间这两个字段本来就是空的，返回 true——连不上网时
*   `AT+REBOOT` 不该被链路状态拖住。
*/
bool Modem_IsUplinkIdle(void);

#endif /* APP_MODEM_H */
