/*============================================================================
* ble.h —— 应用层 BLE 活动对象与采样快照事件
* 层次定位：App 层；以事件驱动可移植 BLE 组件，不向组件泄漏 QP。
*===========================================================================*/
#ifndef APP_BLE_H
#define APP_BLE_H

#include "qpc.h"
#include <stdint.h>

typedef struct {
    QEvt super;
    int32_t temperature_cC;
    uint16_t humidity_permil;
} BleMeasurementEvt;

void Ble_ctor(void);
void Ble_start(void);
void Ble_uartRxIsrSink(void);
extern QActive * const AO_Ble;

#endif
