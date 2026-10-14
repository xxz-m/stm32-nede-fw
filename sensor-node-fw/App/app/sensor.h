/* App 层：请求式采样 AO 接口；STM32F103，QP/C QV。 */
#ifndef SENSOR_H
#define SENSOR_H
#include "qpc.h"
#include "sensor_cycle.h"
typedef struct {
    QActive super;
    QTimeEvt processEvt;
    SensorCycle cycle;
} Sensor;
void Sensor_ctor(void);
void Sensor_start(void);
QState Sensor_initial(Sensor * const me, void const * const par);
QState Sensor_state_Idle(Sensor * const me, QEvt const * const e);
QState Sensor_state_Sampling(Sensor * const me, QEvt const * const e);
/** @brief 投递一次采样请求，targets 为 CLOUD 或 BLE；满队列返回 false。 */
bool Sensor_RequestSample(uint8_t targets);
extern QActive * const AO_Sensor;
#endif
