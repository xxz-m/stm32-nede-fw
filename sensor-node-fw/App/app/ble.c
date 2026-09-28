/*============================================================================
* ble.c —— AO_Ble：VG6328 配置、协议推进与周期上报
* 层次定位：App 层；QP/QV 适配只存在于本文件，组件保持平台无关。
*===========================================================================*/
#include "ble.h"
#include "app_config.h"
#include "app_prio.h"
#include "app_signals.h"
#include "ble_component.h"
#include "bsp.h"
#include "store.h"
#include "svc_log.h"

Q_DEFINE_THIS_MODULE("ble")
void Q_onAssert(char const * const module, int loc);

/**
 * @brief AO_Ble 的完整运行对象。
 *
 * 该结构体同时保存：
 * 1. QP/C 活动对象本体；
 * 2. BLE 可移植组件及其 UART 端口；
 * 3. 蓝牙配置与对外数据参数；
 * 4. 非阻塞状态机的计时器、重试次数和运行标志。
 *
 * AO_Ble 每次收到 BLE_PROCESS_SIG、BLE_RX_SIG 或
 * BLE_MEASUREMENT_SIG 时，都通过这里保存的上下文继续推进工作。
 *
 * 注意：super 必须放在第一个成员，便于将 Ble 对象作为 QActive 使用。
 */
typedef struct {
    QActive super;              /* QP/C 活动对象基类，必须是第一个成员 */
    QTimeEvt processEvt;        /* BLE 周期推进时间事件 */

    BleComponent component;     /* 可移植 BLE 业务组件 */
    BlePort port;               /* BLE 平台端口：UART 收发、时基和日志 */
    BleConfig config;           /* BLE 模组配置，来源于参数存储 */
    BleParam params[5];         /* BLE 对外参数：频率、温度、信号等 */

    uint32_t reportTicks;       /* 周期上报计数 */
    bool failureLogged;         /* 是否已经打印失败日志 */
    bool started;               /* 是否已完成启动等待并开始探测 */
    bool readyLogged;           /* 是否已经打印配置完成日志 */
    bool restartLogged;         /* 是否已经打印模组重启日志 */
    bool sessionReady;          /* BLE 会话是否已经建立并认证 */
    bool rebootLogged;          /* 是否已经打印远程重启日志 */

    uint8_t startupTicks;       /* 启动等待计数 */
    uint8_t retryTicks;         /* 失败重试等待计数 */
    uint8_t attempts;           /* 模组探测/配置尝试次数 */
    uint8_t rebootTicks;        /* 远程重启延时计数 */
} Ble;

static Ble l_ble;
QActive * const AO_Ble = &l_ble.super;
static QEvt const *l_queue[12];
static QState active(Ble * const me, QEvt const * const e);
//每 100 ms 产生一次 BLE_PROCESS_SIG
#define BLE_PROCESS_TICKS ((APP_CFG_BLE_PROCESS_PERIOD_MS * BSP_TICKS_PER_SEC) / 1000U) /* 100 ms */
//等待蓝牙模组启动500ms
#define BLE_STARTUP_TICKS ((APP_CFG_BLE_STARTUP_DELAY_MS + APP_CFG_BLE_PROCESS_PERIOD_MS - 1U) / APP_CFG_BLE_PROCESS_PERIOD_MS) /*500 ms*/
//探测失败后等待 2 s 再重试
#define BLE_RETRY_TICKS   ((APP_CFG_BLE_PROBE_RETRY_MS + APP_CFG_BLE_PROCESS_PERIOD_MS - 1U) / APP_CFG_BLE_PROCESS_PERIOD_MS) /*2s*/

/** @brief 把可移植组件 trace 适配到现有异步日志。 */
static void componentTrace(void *context, char const *text) {
    (void)context;
    LOG_D("ble-core: %s", text);
}

static void paramInit(Ble *me) {
    me->params[0] = (BleParam){"frequency", "Frequency", "Hz", {BLE_VALUE_INT, {.integer = 0}}, false, 0, 100000, 0, 0};
    me->params[1] = (BleParam){"temperature", "Temperature", "C", {BLE_VALUE_DECIMAL, {.decimal = {0, 2U}}}, false, -4000, 12500, 0, 0};
    me->params[2] = (BleParam){"signal_quality", "Signal quality", "%", {BLE_VALUE_INT, {.integer = 0}}, false, 0, 100, 0, 0};
    me->params[3] = (BleParam){"voltage", "Voltage", "mV", {BLE_VALUE_INT, {.integer = 3300}}, false, 0, 5000, 0, 0};
    me->params[4] = (BleParam){"sample_interval", "Sample interval", "s", {BLE_VALUE_INT, {.integer = APP_CFG_BLE_REPORT_PERIOD_S}}, true, 1, 86400, 0, 0};
}

static QState initial(Ble * const me, void const * const par) {
    (void)par;
    return Q_TRAN(&active);
}

static QState active(Ble * const me, QEvt const * const e) {
    switch (e->sig) {
        case Q_ENTRY_SIG://初始化
            QTimeEvt_armX(&me->processEvt, BLE_PROCESS_TICKS, BLE_PROCESS_TICKS);
            /* NOTE (打印实际生效值)：与 4G 端口同一规矩——读外设句柄里的速率，
            *   而不是 APP_CFG_BLE_UART_BAUD 宏（宏没被应用时日志会说谎）。 */
            LOG_I("ble: VG6328 on USART%u %lu 8N1, waiting for module startup",
                  (unsigned)APP_CFG_BLE_UART_ID,
                  (unsigned long)BSP_bleUartBaud());
            return Q_HANDLED();
        case BLE_PROCESS_SIG: {//每 100 ms 周期触发
            uint32_t period;
            static char const *const names[] = {"frequency", "temperature", "signal_quality", "voltage", "sample_interval"};
            if (!me->started) {
                /* NOTE (等配置就绪)：本地名、Service UUID、PIN 都来自参数存储，
                * 必须等介质里的记录读回来之后再探测模组，否则会把编译期默认值
                * 写进模组（现场表现为「改了 SN 不生效」）。存储关闭或器件不在时
                * App_Store_ConfigResolved() 也会很快变真，不会把 BLE 永久卡住。 */
                if (!App_Store_ConfigResolved()) return Q_HANDLED();
                if (++me->startupTicks < BLE_STARTUP_TICKS) return Q_HANDLED();
                me->started = true;
                me->attempts = 1U;
								//启动蓝牙业务组件的探测、配置和连接状态机
                BLE_Component_Start(&me->component);
                LOG_I("ble: probing VG6328, attempt 1/%u",
                      (unsigned)APP_CFG_BLE_PROBE_ATTEMPTS);
            }
            BLE_Component_Process(&me->component);
						//模组正在重启（组件第 13 步后主动等待 500 ms）
            if (me->component.restart_waiting && !me->restartLogged) {
                LOG_I("ble: restarting VG6328 to apply profile");
                me->restartLogged = true;
            }
						//已认证客户端下发重启命令（ble_protocol.c 里置位）
            if (me->component.reboot_pending) {
                if (!me->rebootLogged) {
                    LOG_W("ble: authenticated client requested system reboot");
                    me->rebootLogged = true;
                }
								//复位是不可逆动作，先等 1 s 是为了让重启命令的回复、日志和串口发送队列都有机会真正发出去，否则客户端只看到设备凭空消失。
                if (++me->rebootTicks >= 10U) BSP_systemReset();
            } else {
                me->rebootTicks = 0U;
                me->rebootLogged = false;
            }//组件探测/配置失败
            if (me->component.state == BLE_COMPONENT_FAILED) {
                if (!me->failureLogged) {
                    LOG_W("ble: probe failed reason=%u step=%u tx=%lu rx=%lu uart_err=%lu overflow=%lu",
                          (unsigned)me->component.failure,
                          (unsigned)me->component.failure_step,
                          (unsigned long)BSP_bleUartTxCount(),
                          (unsigned long)BSP_bleUartRxCount(),
                          (unsigned long)BSP_bleUartErrorCount(),
                          (unsigned long)BSP_bleUartOverflow());
                    me->failureLogged = true;
                }
                if ((me->attempts < APP_CFG_BLE_PROBE_ATTEMPTS) &&
                    (++me->retryTicks >= BLE_RETRY_TICKS)) {
                    ++me->attempts;
                    me->retryTicks = 0U;
                    me->failureLogged = false;
                    BLE_Component_Start(&me->component);
                    LOG_I("ble: probing VG6328, attempt %u/%u",
                          (unsigned)me->attempts,
                          (unsigned)APP_CFG_BLE_PROBE_ATTEMPTS);
                }
										//首次配置成功
            } else if ((me->component.state == BLE_COMPONENT_READY) && !me->readyLogged) {
                LOG_I("ble: VG6328 configured, mac=%s, waiting for connection", me->component.mac_address);
                me->readyLogged = true;
            }
						//组件是否已连接 + 已认证
            if (!BLE_Component_IsReady(&me->component)) {
                me->sessionReady = false;
            } else if (!me->sessionReady) {
                me->sessionReady = true;
                me->reportTicks = 0U;
                if (BLE_Component_Report(&me->component, names, Q_DIM(names))) {
                    LOG_I("ble: authenticated, initial data report queued");
                } else {
                    LOG_W("ble: initial data report queue busy");
                }
            } else {
                period = ((uint32_t)me->params[4].value.value.integer * 1000U) /
                         APP_CFG_BLE_PROCESS_PERIOD_MS;
                if (++me->reportTicks >= period) {
                    me->reportTicks = 0U;
                    (void)BLE_Component_Report(&me->component, names, Q_DIM(names));
                }
            }
            return Q_HANDLED();
        }
        case BLE_RX_SIG:
					//这个是回调的入口函数
            BLE_Component_Process(&me->component);
            return Q_HANDLED();
        case BLE_MEASUREMENT_SIG: {
            BleMeasurementEvt const *snapshot = (BleMeasurementEvt const *)e;
            me->params[1].value.value.decimal.mantissa = snapshot->temperature_cC;
            return Q_HANDLED();
        }
        default:
            return Q_SUPER(&QHsm_top);
    }
}

void Ble_uartRxIsrSink(void) {
    static QEvt const event = {(QSignal)BLE_RX_SIG, 0U, 0U};
    (void)QACTIVE_POST_X(AO_Ble, &event, 0U, 0U);
}

void Ble_ctor(void) {
    Ble *me = &l_ble;
    QActive_ctor(&me->super, Q_STATE_CAST(&initial));
    QTimeEvt_ctorX(&me->processEvt, &me->super, BLE_PROCESS_SIG, 0U);
    /* NOTE (产品身份来源，2026-09-27 改)：这些值不再是**直接**读编译期宏，而是读
    * 参数影子（AO_Store 持有）。影子在上电时先落 app_config.h 的默认值，
    * 随后 AO_Store 把 I2C EEPROM 里的有效记录读回来**就地覆盖**；BleConfig 持有的是
    * 影子内部地址，因此覆盖之后这里读到的就是介质里的值。
    * 顺序保证：装配点先调 Store_ctor()，本函数在其之后；而真正把配置写进模组的
    * 时刻被 `App_Store_ConfigResolved()` 门控（见 BLE_PROCESS_SIG 分支），
    * 所以不会出现「拿默认值配了模组、随后又读到真值」的错配。 */
    me->config = (BleConfig){
        App_Store_Value(SVC_PARAM_BLE_SN),
        App_Store_Value(SVC_PARAM_BLE_PIN),
        App_Store_Value(SVC_PARAM_BLE_NAME),
        App_Store_Value(SVC_PARAM_BLE_UUID),
        App_Store_Value(SVC_PARAM_BLE_DEVNAME),
        App_Store_Value(SVC_PARAM_BLE_MODEL),
        App_Store_Value(SVC_PARAM_BLE_BATCH)
    };
    paramInit(me);
    me->reportTicks = 0U;
    me->failureLogged = false;
    me->started = false;
    me->readyLogged = false;
    me->restartLogged = false;
    me->sessionReady = false;
    me->rebootLogged = false;
    me->startupTicks = 0U;
    me->retryTicks = 0U;
    me->attempts = 0U;
    me->rebootTicks = 0U;
    BSP_bleUartInit(&me->port, (bsp_uart_id_t)APP_CFG_BLE_UART_ID);
    me->port.trace = &componentTrace;
    BLE_Component_Init(&me->component, &me->port, &me->config, me->params, Q_DIM(me->params));
}

void Ble_start(void) {
    QACTIVE_START(AO_Ble, APP_BLE_PRIO, l_queue, Q_DIM(l_queue),
                  (void *)0, 0U, (QEvt *)0);
}
