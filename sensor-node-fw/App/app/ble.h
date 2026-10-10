/*============================================================================
* ble.h —— 应用层 BLE 活动对象与采样快照事件
* 层次定位：App 层；以事件驱动可移植 BLE 组件，不向组件泄漏 QP。
*===========================================================================*/
#ifndef APP_BLE_H
#define APP_BLE_H

#include "qpc.h"
#include <stdint.h>

/*! AO_Sensor 发来的采样快照。
*  NOTE (2026-10-10)：湿度通道已随仿真源一起删除——SCL3300 没有湿度，
*  改为三轴倾角（0.01° 标度）+ 片内温度（0.01 °C）。 */
typedef struct {
    QEvt super;
    int32_t temperature_cC;   /* 片内温度，0.01 °C */
    int32_t angle_x_cdeg;     /* X 轴倾角，0.01°   */
    int32_t angle_y_cdeg;     /* Y 轴倾角，0.01°   */
    int32_t angle_z_cdeg;     /* Z 轴倾角，0.01°   */
} BleMeasurementEvt;

void Ble_ctor(void);
void Ble_start(void);
void Ble_uartRxIsrSink(void);
extern QActive * const AO_Ble;

#endif
