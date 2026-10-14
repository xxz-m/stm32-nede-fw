/* App 层：AO_Sensor 请求式采样调度，STM32F103/SCL3300/电池，QP/C QV。
 * 不自行按秒采样、不打印测量数据；通信 AO 按业务周期请求，完整结果按值投递。
 * 初始化和器件恢复仍由 20ms 时间事件推进，禁止等待总线或模组应答。 */
#include "sensor.h"
#include "app_config.h"
#include "app_signals.h"
#include "app_prio.h"
#include "bsp.h"
#include "drv_battery.h"
#include "svc_log.h"
#include "ble.h"
#include "modem.h"
#include "node.h"
Q_DEFINE_THIS_MODULE("sensor")
void Q_onAssert(char const * const module, int loc);
static Sensor l_sensor;
QActive * const AO_Sensor = &l_sensor.super;
static QEvt const *l_queue[8];
#define SENSOR_PROCESS_TICKS ((APP_CFG_SCL3300_PROCESS_PERIOD_MS * BSP_TICKS_PER_SEC) / 1000U)
/** @brief 对两个通信 AO 发不可变结果，失败同样通知以释放等待状态。 */
static void deliver(SensorCycleResult const *r) {
    unsigned axis;
    NodeMeasurementEvt *n = Q_NEW(NodeMeasurementEvt, SYS_NODE_MEASUREMENT_SIG);
    for (axis = 0U; axis < 3U; ++axis) {
        n->snapshot.angle_cdeg[axis] = r->sensor.angle_cdeg[axis];
        n->snapshot.acc_mg[axis] = r->sensor.acc_mg[axis];
    }
    n->snapshot.temperature_cC = r->sensor.temp_cC;
    n->snapshot.battery_mV = r->battery_mV;
    n->snapshot.battery_valid = r->battery_valid;
    n->snapshot.valid = r->valid;
    QACTIVE_POST(AO_Node, &n->super, &l_sensor);
#if APP_CFG_BLE_ENABLE != 0U
    BleMeasurementEvt *b = Q_NEW(BleMeasurementEvt, BLE_MEASUREMENT_SIG);
    b->temperature_cC = r->sensor.temp_cC;
    b->angle_x_cdeg = r->sensor.angle_cdeg[0]; b->angle_y_cdeg = r->sensor.angle_cdeg[1];
    b->angle_z_cdeg = r->sensor.angle_cdeg[2]; b->battery_mV = r->battery_mV;
    b->battery_valid = r->battery_valid; b->valid = r->valid;
    QACTIVE_POST(AO_Ble, &b->super, &l_sensor);
#endif
#if APP_CFG_MODEM_ENABLE != 0U
    ModemMeasurementEvt *m = Q_NEW(ModemMeasurementEvt, MODEM_MEASUREMENT_SIG);
    m->temperature_cC = r->sensor.temp_cC;
    m->angle_x_cdeg = r->sensor.angle_cdeg[0]; m->angle_y_cdeg = r->sensor.angle_cdeg[1];
    m->angle_z_cdeg = r->sensor.angle_cdeg[2]; m->battery_mV = r->battery_mV;
    m->battery_valid = r->battery_valid; m->valid = r->valid;
    for (axis = 0U; axis < 3U; ++axis) { m->acc_mg[axis] = r->sensor.acc_mg[axis]; }
    m->cloud_requested = (r->targets & SENSOR_TARGET_CLOUD) != 0U;
    QACTIVE_POST(AO_Modem, &m->super, &l_sensor);
#endif
}
QState Sensor_state_Idle(Sensor * const me, QEvt const * const e) {
    (void)me;
    if (e->sig == SYS_SENSOR_START_SIG) { return Q_TRAN(&Sensor_state_Sampling); }
    return Q_SUPER(&QHsm_top);
}
QState Sensor_state_Sampling(Sensor * const me, QEvt const * const e) {
    switch (e->sig) {
        case Q_ENTRY_SIG:
            QTimeEvt_armX(&me->processEvt, SENSOR_PROCESS_TICKS, SENSOR_PROCESS_TICKS);
            return Q_HANDLED();
        case Q_EXIT_SIG:
            QTimeEvt_disarm(&me->processEvt); App_SensorCycle_Cancel(&me->cycle); return Q_HANDLED();
        case SYS_SENSOR_SAMPLE_CLOUD_SIG: case SYS_SENSOR_REPORT_SIG:
            App_SensorCycle_Request(&me->cycle, SENSOR_TARGET_CLOUD); return Q_HANDLED();
        case SYS_SENSOR_SAMPLE_BLE_SIG:
            App_SensorCycle_Request(&me->cycle, SENSOR_TARGET_BLE); return Q_HANDLED();
        case SENSOR_PROCESS_SIG: {
            SensorCycleResult result;
            App_SensorCycle_Process(&me->cycle);
            if (App_SensorCycle_Take(&me->cycle, &result)) { deliver(&result); }
            return Q_HANDLED();
        }
        case SYS_SENSOR_STOP_SIG: return Q_TRAN(&Sensor_state_Idle);
        default: return Q_SUPER(&QHsm_top);
    }
}
QState Sensor_initial(Sensor * const me, void const * const par) {
    (void)me; (void)par; return Q_TRAN(&Sensor_state_Idle);
}
void Sensor_ctor(void) {
    Sensor *me = &l_sensor;
    QActive_ctor(&me->super, Q_STATE_CAST(&Sensor_initial));
    QTimeEvt_ctorX(&me->processEvt, &me->super, SENSOR_PROCESS_SIG, 0U);
    App_SensorCycle_Init(&me->cycle);
}
void Sensor_start(void) {
#if APP_CFG_BATTERY_ENABLE != 0U
    if (DRV_Battery_Init() != DRV_OK) { LOG_W("battery: init failed"); }
#endif
    if (DRV_Sensor_Init() != DRV_OK) { LOG_W("sensor: init failed"); }
    QACTIVE_START(AO_Sensor, APP_SENSOR_PRIO, l_queue, Q_DIM(l_queue), (void *)0, 0U, (QEvt *)0);
}
bool Sensor_RequestSample(uint8_t targets) {
    static QEvt const cloud = {(QSignal)SYS_SENSOR_SAMPLE_CLOUD_SIG, 0U, 0U};
    static QEvt const ble = {(QSignal)SYS_SENSOR_SAMPLE_BLE_SIG, 0U, 0U};
    QEvt const *event = targets == SENSOR_TARGET_CLOUD ? &cloud : &ble;
    return QACTIVE_POST_X(AO_Sensor, event, 0U, (void *)0);
}
