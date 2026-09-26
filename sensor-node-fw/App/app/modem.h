/*============================================================================
* modem.h —— 应用层 4G 活动对象与采样快照事件
* 层次定位：App 层；以事件驱动可移植 4G 组件，不向组件泄漏 QP。
*===========================================================================*/
#ifndef APP_MODEM_H
#define APP_MODEM_H

#include "qpc.h"
#include <stdint.h>

/*! AO_Sensor 发来的采样快照；只带组件需要的两个物理量。 */
typedef struct {
    QEvt super;
    int32_t temperature_cC;   /* 温度，0.01 °C   */
    uint16_t humidity_permil; /* 相对湿度，0.1 %RH */
} ModemMeasurementEvt;

void Modem_ctor(void);
void Modem_start(void);
void Modem_uartRxIsrSink(void);
extern QActive * const AO_Modem;

#endif /* APP_MODEM_H */
