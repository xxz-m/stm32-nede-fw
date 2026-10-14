/* App 层：AO_Ble，VG6328 配置、限时广播、认证后请求采样并发送。
 * STM32F103 / QP/C QV；UART 与电源经 BSP，广播窗口由ble_runtime管理，周期由统一上报调度管理。
 * Driver 不知道蓝牙事件；BLE 组件不依赖 QP、HAL 或业务采样函数。 */
#include "ble.h"
#include "app_config.h"
#if APP_CFG_BLE_ENABLE != 0U
#include "app_prio.h"
#include "app_signals.h"
#include "bsp.h"
#include "store.h"
#include "sensor.h"
#include "modem.h"
#include "svc_log.h"
#include "ble_runtime.h"
#include "report_period.h"
Q_DEFINE_THIS_MODULE("ble")
void Q_onAssert(char const * const module, int loc);
typedef struct {
    QActive super;
    QTimeEvt processEvt;
    BleComponent component;
    BlePort port;
    BleConfig config;
    BleRuntime runtime;
    uint32_t startupTicks;
    uint32_t retryTicks;
    uint8_t attempts;
    uint8_t rebootTicks;
    bool started;
    bool failureLogged;
    bool readyLogged;
    bool rebootLogged;
} Ble;
static Ble l_ble;
QActive * const AO_Ble = &l_ble.super;
static QEvt const *l_queue[12];
#define BLE_PROCESS_TICKS ((APP_CFG_BLE_PROCESS_PERIOD_MS * BSP_TICKS_PER_SEC) / 1000U)
#define BLE_STARTUP_TICKS ((APP_CFG_BLE_STARTUP_DELAY_MS + APP_CFG_BLE_PROCESS_PERIOD_MS - 1U) / APP_CFG_BLE_PROCESS_PERIOD_MS)
#define BLE_RETRY_TICKS ((APP_CFG_BLE_PROBE_RETRY_MS + APP_CFG_BLE_PROCESS_PERIOD_MS - 1U) / APP_CFG_BLE_PROCESS_PERIOD_MS)
/** @brief 组件 trace 只通过异步日志输出。 */
static void componentTrace(void *context, char const *text) {
    (void)context; LOG_D("ble-core: %s", text);
}
/** @brief 接收数据只推进组件，不在中断或回调里执行协议。 */
static void process(Ble *me) {
    char const *sent;
    uint32_t now = BSP_uptimeMs();
    /* 在协议get/params之前刷新镜像，串口或云端AT修改也立即可见。 */
    me->runtime.params[6].value.value.integer = (int32_t)App_ReportPeriod_Minutes();
    BLE_Component_Process(&me->component);
    sent = BLE_Component_TakeSentReport(&me->component);
    if (sent != (char const *)0) {
        LOG_WRITE("ble> sent "); LOG_WRITE(sent); LOG_WRITE("\r\n");
    }
    if (App_BleRuntime_Process(&me->runtime, &me->component, now)) {
#if APP_CFG_MODEM_ENABLE != 0U
        if (!Modem_RequestReport()) { me->runtime.session_ready = false; }
#else
        if (!Sensor_RequestSample(SENSOR_TARGET_CLOUD)) { me->runtime.session_ready = false; }
#endif
    }
}
static QState active(Ble * const me, QEvt const * const e) {
    switch (e->sig) {
        case Q_ENTRY_SIG:
            QTimeEvt_armX(&me->processEvt, BLE_PROCESS_TICKS, BLE_PROCESS_TICKS);
            LOG_I("ble: waiting for module startup; advertising disabled");
            return Q_HANDLED();
        case SYS_BLE_ADVERT_REQUEST_SIG:
            App_BleRuntime_OpenWindow(&me->runtime, BSP_uptimeMs());
            LOG_I("ble: advertising window requested, duration=%lu ms", (unsigned long)APP_CFG_BLE_ADVERT_WINDOW_MS);
            return Q_HANDLED();
        case BLE_PROCESS_SIG:
            if (!me->started) {
                if (!App_Store_ConfigResolved() || ++me->startupTicks < BLE_STARTUP_TICKS) { return Q_HANDLED(); }
                me->started = true; me->attempts = 1U;
                BLE_Component_Start(&me->component);
            }
            process(me);
            if (me->component.reboot_pending) {
                if (!me->rebootLogged) { LOG_W("ble: authenticated client requested reboot"); me->rebootLogged = true; }
                if (++me->rebootTicks >= 10U) { BSP_systemReset(); }
            } else { me->rebootTicks = 0U; me->rebootLogged = false; }
            if (me->component.state == BLE_COMPONENT_FAILED) {
                if (!me->failureLogged) {
                    LOG_W("ble: module failed reason=%u step=%u rx=%lu tx=%lu overflow=%lu uart_err=%lu",
                        (unsigned)me->component.failure, (unsigned)me->component.failure_step,
                        (unsigned long)BSP_bleUartRxCount(), (unsigned long)BSP_bleUartTxCount(),
                        (unsigned long)BSP_bleUartOverflow(), (unsigned long)BSP_bleUartErrorCount());
                    me->failureLogged = true;
                }
                if (me->attempts < APP_CFG_BLE_PROBE_ATTEMPTS && ++me->retryTicks >= BLE_RETRY_TICKS) {
                    ++me->attempts; me->retryTicks = 0U; me->failureLogged = false;
                    me->readyLogged = false; BLE_Component_Start(&me->component);
                }
            } else if (me->component.state == BLE_COMPONENT_READY && !me->readyLogged) {
                me->readyLogged = true;
                LOG_I("ble: configured, mac=%s", me->component.mac_address);
            }
            return Q_HANDLED();
        case BLE_RX_SIG:
            process(me); return Q_HANDLED();
        case SYS_BLE_SIGNAL_SIG:
            me->runtime.params[4].value.value.integer = ((BleSignalEvt const *)e)->signal;
            return Q_HANDLED();
        case BLE_MEASUREMENT_SIG: {
            BleMeasurementEvt const *s = (BleMeasurementEvt const *)e;
            if (BLE_Component_IsReady(&me->component)) {
                App_BleRuntime_Measurement(&me->runtime, s->temperature_cC,
                    s->angle_x_cdeg, s->angle_y_cdeg, s->angle_z_cdeg,
                    s->battery_mV, s->battery_valid, s->valid);
                if (!s->valid) { LOG_W("ble: fresh sample unavailable, data skipped"); }
            }
            return Q_HANDLED();
        }
        default: return Q_SUPER(&QHsm_top);
    }
}
static QState initial(Ble * const me, void const * const par) {
    (void)me; (void)par; return Q_TRAN(&active);
}
void Ble_ctor(void) {
    Ble *me = &l_ble;
    QActive_ctor(&me->super, Q_STATE_CAST(&initial));
    QTimeEvt_ctorX(&me->processEvt, &me->super, BLE_PROCESS_SIG, 0U);
    me->config = (BleConfig){App_Store_Value(SVC_PARAM_BLE_SN), App_Store_Value(SVC_PARAM_BLE_PIN),
        App_Store_Value(SVC_PARAM_BLE_SN), App_Store_Value(SVC_PARAM_BLE_UUID),
        App_Store_Value(SVC_PARAM_BLE_DEVNAME), App_Store_Value(SVC_PARAM_BLE_MODEL), App_Store_Value(SVC_PARAM_BLE_BATCH)};
    App_BleRuntime_Init(&me->runtime);
    BSP_bleUartInit(&me->port, (bsp_uart_id_t)APP_CFG_BLE_UART_ID);
    me->port.trace = componentTrace;
    BLE_Component_Init(&me->component, &me->port, &me->config, me->runtime.params, Q_DIM(me->runtime.params));
}
void Ble_start(void) {
    QACTIVE_START(AO_Ble, APP_BLE_PRIO, l_queue, Q_DIM(l_queue), (void *)0, 0U, (QEvt *)0);
}
void Ble_uartRxIsrSink(void) {
    static QEvt const event = {(QSignal)BLE_RX_SIG, 0U, 0U};
    (void)QACTIVE_POST_X(AO_Ble, &event, 0U, (void *)0);
}
void Ble_RequestAdvertising(void) {
    static QEvt const event = {(QSignal)SYS_BLE_ADVERT_REQUEST_SIG, 0U, 0U};
    (void)QACTIVE_POST_X(AO_Ble, &event, 0U, (void *)0);
}
#endif
