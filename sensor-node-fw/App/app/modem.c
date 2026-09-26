/*============================================================================
* modem.c —— AO_Modem：4G 模组入网、云端 MQTT 维持与周期上报
* 层次定位：App 层；QP/QV 适配只存在于本文件，组件保持平台无关。
*
* 非阻塞推进方式（与 AO_Ble 同构）：
*   - MODEM_PROCESS_SIG：APP_CFG_MODEM_PROCESS_PERIOD_MS 的时间事件，周期调
*     Modem_Component_Process()，由组件内部按期限推进 AT 命令；
*   - MODEM_RX_SIG：模组串口（`APP_CFG_MODEM_UART_ID` 指定的那一路）收到字节时由
*     BSP 的 sink 投递，再调一次 Process 尽快吃掉；
*   - MODEM_MEASUREMENT_SIG：AO_Sensor 的采样快照，只更新组件里的数据字段。
* 三种事件都只做「喂进去 + 立刻返回」，没有任何就地等待。
*===========================================================================*/
#include "modem.h"
/* NOTE (顺序)：app_config.h 必须先于守卫被包含，否则 APP_CFG_MODEM_ENABLE 未定义、
* 在预处理里按 0 处理，开了功能也照样把整个模块编译掉。 */
#include "app_config.h"

/* 关掉 4G 时整个模块在编译期消失：bsp_modem_uart.h 同样只在
* APP_CFG_MODEM_ENABLE != 0 时声明接口，这里若不留守卫会出现隐式声明。
* 装配、事件池与采样快照投递的守卫在 app.c / sensor.c。 */
#if (APP_CFG_MODEM_ENABLE != 0U)

#include "app_prio.h"
#include "app_signals.h"
#include "bsp.h"
#include "modem_component.h"
#include "svc_log.h"

Q_DEFINE_THIS_MODULE("modem")
void Q_onAssert(char const * const module, int loc);

typedef struct {
    QActive super;
    QTimeEvt processEvt;
    ModemComponent component;
    ModemPort port;
    ModemConfig config;
    uint32_t reportTicks;    /* 距离下次周期上报的 Process 计数 */
    uint32_t cellTicks;      /* 距离下次查小区信息的 Process 计数 */
    uint16_t retryTicks;     /* 失败后距重连的 Process 计数     */
    uint16_t powerOffTicks;  /* 断电重启：已断电的 Process 计数 */
    uint8_t startupTicks;    /* 上电等待模组启动的 Process 计数 */
    uint8_t attempts;        /* 已尝试的连接次数                */
    bool started;
    bool powerCut;           /* true = 正在"断电"窗口内         */
    bool failureLogged;
    bool readyLogged;
} Modem;

static Modem l_modem;
QActive * const AO_Modem = &l_modem.super;
static QEvt const *l_queue[12];
static QState active(Modem * const me, QEvt const * const e);

/* 时间参数统一由毫秒配置换算成 Process 周期数，避免散落裸数字。 */
#define MODEM_PROCESS_TICKS ((APP_CFG_MODEM_PROCESS_PERIOD_MS * BSP_TICKS_PER_SEC) / 1000U)
#define MODEM_PERIOD_TICKS(ms_) \
    (((ms_) + APP_CFG_MODEM_PROCESS_PERIOD_MS - 1U) / APP_CFG_MODEM_PROCESS_PERIOD_MS)
#define MODEM_STARTUP_TICKS MODEM_PERIOD_TICKS(APP_CFG_MODEM_STARTUP_DELAY_MS)
#define MODEM_RETRY_TICKS   MODEM_PERIOD_TICKS(APP_CFG_MODEM_RECONNECT_PERIOD_S * 1000U)
#define MODEM_REPORT_TICKS  MODEM_PERIOD_TICKS(APP_CFG_MODEM_REPORT_PERIOD_S * 1000U)
#define MODEM_CELL_TICKS    MODEM_PERIOD_TICKS(APP_CFG_MODEM_CELL_REFRESH_PERIOD_S * 1000U)
/*! 断电重启的断电时长（Process 周期数）；上电后的等待复用"等模组启动"那一段。 */
#define MODEM_POWER_OFF_TICKS \
    MODEM_PERIOD_TICKS(APP_CFG_MODEM_POWER_OFF_MS)

/** @brief 把可移植组件 trace 适配到现有异步日志。 */
static void componentTrace(void *context, char const *text) {
    (void)context;
    LOG_D("4g-core: %s", text);
}

/** @brief 连接失败时打印一次诊断读数。 */
static void logFailure(Modem *me) {
    /* NOTE (rx 字段，2026-09-27 加)：uart_err 只能说明「收到了畸形字节」，
    *   区分不了「线上一片安静」和「对方根本不应答」——而波特率配错正好属于后者
    *   （模组听不懂就不会回，我们这边也收不到任何字节、uart_err 一直是 0）。
    *   加上 rx（累计收到字节数）后：rx=0 且 uart_err=0 = 线上一片安静；
    *   rx>0 且 uart_err>0 = 字节来了但成帧失败，优先怀疑波特率。 */
    LOG_W("modem: link failed reason=%u step=%u rx=%lu tx_ovf=%lu uart_err=%lu",
          (unsigned)me->component.failure,
          /* NOTE (step 口径，2026-09-27)：`failure_step` 存的是**从 0 开始**的计划下标，
           *   而 trace 打的是 `step k/N`（从 1 开始）。直接印会出现「trace 说第 18 步失败、
           *   这行却说 step=17」的矛盾，现场要核对半天。这里 +1 与 trace 对齐。 */
          (unsigned)me->component.failure_step + 1U,
          (unsigned long)BSP_modemUartRxCount(),
          (unsigned long)BSP_modemOverflowCount(),
          (unsigned long)BSP_modemErrorCount());
}

/** @brief 进入在线态时打印一次。 */
static void logReady(Modem *me) {
    LOG_I("modem: cloud link ready, report every %lu s",
          (unsigned long)APP_CFG_MODEM_REPORT_PERIOD_S);
    LOG_I("modem: signal=%ld cell_tac=%s cell_id=%s",
          (long)me->component.data.signal_quality,
          (me->component.data.tac[0] != '\0') ? me->component.data.tac : "-",
          (me->component.data.cell_id[0] != '\0') ? me->component.data.cell_id : "-");
}

/** @brief 发起一次连接（首次启动与失败重连共用）。 */
static void connect(Modem *me) {
    me->component.data.tac[0] = '\0';
    me->component.data.cell_id[0] = '\0';
    me->component.data.location_valid = false;
    me->reportTicks = 0U;
    me->cellTicks = 0U;
    me->retryTicks = 0U;
    me->failureLogged = false;
    me->readyLogged = false;
    Modem_Component_Start(&me->component);
}

static QState initial(Modem * const me, void const * const par) {
    (void)par;
    return Q_TRAN(&active);
}

static QState active(Modem * const me, QEvt const * const e) {
    switch (e->sig) {
        case Q_ENTRY_SIG:
            QTimeEvt_armX(&me->processEvt, MODEM_PROCESS_TICKS, MODEM_PROCESS_TICKS);
            /* NOTE (打印实际生效值)：这里读的是外设句柄里的速率（BSP_modemUartBaud()），
            *   不是 APP_CFG_MODEM_UART_BAUD 宏。2026-09-27 之前打的是宏，而宏从未被写进
            *   外设——日志写着 9600、硬件跑 115200，把真正的故障掩盖了整整一轮排查。 */
            LOG_I("modem: %s on USART%u %lu 8N1, waiting for module startup",
                  APP_CFG_MODEM_BACKEND_NAME,
                  (unsigned)APP_CFG_MODEM_UART_ID,
                  (unsigned long)BSP_modemUartBaud());
            return Q_HANDLED();
        case MODEM_PROCESS_SIG: {
            /* NOTE (断电重启，2026-09-20)：连不上时给模组彻底掉电再上电——这是唯一能
             *   清掉「掉网后不再搜网」这类卡死状态的手段（实机实测：模组掉网后停在
             *   `+CEREG:2,0,,,` 十分钟，既不注册也不搜网，射频软复位也没救回来）。
             *   全程不阻塞：断电时长靠本 20 ms 周期事件数拍子。 */
            if (me->powerCut) {
                if (++me->powerOffTicks < MODEM_POWER_OFF_TICKS) {
                    return Q_HANDLED();
                }
                BSP_modemPowerSet(true);
                /* 重装接收：清掉断电期间可能攒下的残字节，避免被当成模组的应答。 */
                BSP_modemUartInit(&me->port, (bsp_uart_id_t)APP_CFG_MODEM_UART_ID);
                me->powerCut = false;
                me->started = false;      /* 重新走一遍"等模组启动"的延时 */
                me->startupTicks = 0U;
                me->attempts = 0U;
                me->failureLogged = false;
                me->readyLogged = false;
                LOG_I("modem: module powered back on, reconnecting");
                return Q_HANDLED();
            }
            if (!me->started) {
                if (++me->startupTicks < MODEM_STARTUP_TICKS) {
                    return Q_HANDLED();
                }
                me->started = true;
                me->attempts = 1U;
                LOG_I("modem: probing 4G module, attempt 1/%u",
                      (unsigned)APP_CFG_MODEM_CONNECT_ATTEMPTS);
                connect(me);
            }
            Modem_Component_Process(&me->component);

            if (me->component.state == MODEM_COMPONENT_FAILED) {
                if (!me->failureLogged) {
                    logFailure(me);
                    me->failureLogged = true;
                }
                if (++me->retryTicks >= MODEM_RETRY_TICKS) {
                    me->retryTicks = 0U;
                    if (me->attempts < APP_CFG_MODEM_CONNECT_ATTEMPTS) {
                        ++me->attempts;
                        LOG_I("modem: reconnecting, attempt %u/%u",
                              (unsigned)me->attempts,
                              (unsigned)APP_CFG_MODEM_CONNECT_ATTEMPTS);
                        connect(me);
                    }
#if (APP_CFG_MODEM_POWER_CYCLE_ENABLE != 0U)
                    else {
                        /* NOTE (不再永久放弃，2026-09-20)：原实现到次数上限就彻底沉默，
                         *   只能人工上电（实测「跑 20 分钟后掉线」正是如此收场）。现在
                         *   改成「断电重启模组，然后从 attempt 1 重新来」，循环往复，
                         *   设备永远有机会自己恢复。 */
                        LOG_W("modem: %u attempts failed, power-cycling the module",
                              (unsigned)APP_CFG_MODEM_CONNECT_ATTEMPTS);
                        BSP_modemPowerSet(false);
                        me->powerCut = true;
                        me->powerOffTicks = 0U;
                    }
#else
                    else {
                        /* 没有电源控制脚时退而求其次：清零计数、继续重试，永不放弃。 */
                        LOG_W("modem: %u attempts failed, retrying from scratch",
                              (unsigned)APP_CFG_MODEM_CONNECT_ATTEMPTS);
                        me->attempts = 0U;
                        connect(me);
                    }
#endif
                }
                return Q_HANDLED();
            }
            if (!Modem_Component_IsReady(&me->component)) {
                return Q_HANDLED();
            }
            if (!me->readyLogged) {
                me->readyLogged = true;
                logReady(me);
            }
            /* 周期上报与小区信息刷新：只置请求位，真正的 AT 往返由组件在
             * 下一个 Process 周期发起，AO 不等待。 */
            if (me->component.report_enabled &&
                (++me->reportTicks >= MODEM_REPORT_TICKS)) {
                me->reportTicks = 0U;
                Modem_Component_RequestReport(&me->component);
            }
            if (++me->cellTicks >= MODEM_CELL_TICKS) {
                me->cellTicks = 0U;
                Modem_Component_RequestCellInfo(&me->component);
                /* 定位与小区信息同频补查：两者都是「从网络侧派生的补充数据」，
                 * 平台也是一起消费的，再开一个计时器没有收益。后端不支持的
                 * （NT26 / MN316）会被组件静默丢掉，不影响这里。 */
                Modem_Component_RequestLocation(&me->component);
            }
            return Q_HANDLED();
        }
        case MODEM_RX_SIG:
            Modem_Component_Process(&me->component);
            return Q_HANDLED();
        case MODEM_MEASUREMENT_SIG: {
            ModemMeasurementEvt const *snapshot = (ModemMeasurementEvt const *)e;
            me->component.data.temperature_cC = snapshot->temperature_cC;
            me->component.data.humidity_permil = snapshot->humidity_permil;
            return Q_HANDLED();
        }
        default:
            return Q_SUPER(&QHsm_top);
    }
}

void Modem_uartRxIsrSink(void) {
    static QEvt const event = {(QSignal)MODEM_RX_SIG, 0U, 0U};
    (void)QACTIVE_POST_X(AO_Modem, &event, 0U, 0U);
}

void Modem_ctor(void) {
    Modem *me = &l_modem;

    QActive_ctor(&me->super, Q_STATE_CAST(&initial));
    QTimeEvt_ctorX(&me->processEvt, &me->super, MODEM_PROCESS_SIG, 0U);
    /* NOTE (凭据)：这里只把凭据交给组件，绝不写进任何日志。 */
    me->config = (ModemConfig){APP_CFG_MODEM_HOST, APP_CFG_MODEM_PORT,
                               APP_CFG_MODEM_PRODUCT_ID, APP_CFG_MODEM_DEVICE_ID,
                               APP_CFG_MODEM_USERNAME, APP_CFG_MODEM_PASSWORD,
                               APP_CFG_MODEM_KEEP_ALIVE_S, APP_CFG_MODEM_REPORT_PERIOD_S};
    me->reportTicks = 0U;
    me->cellTicks = 0U;
    me->retryTicks = 0U;
    me->powerOffTicks = 0U;
    me->startupTicks = 0U;
    me->attempts = 0U;
    me->started = false;
    me->powerCut = false;
    me->failureLogged = false;
    me->readyLogged = false;
    BSP_modemUartInit(&me->port, (bsp_uart_id_t)APP_CFG_MODEM_UART_ID);
    me->port.trace = &componentTrace;
    Modem_Component_Init(&me->component, &me->port, &me->config);
}

void Modem_start(void) {
    QACTIVE_START(AO_Modem, APP_MODEM_PRIO, l_queue, Q_DIM(l_queue),
                  (void *)0, 0U, (QEvt *)0);
}

#endif /* APP_CFG_MODEM_ENABLE != 0U */
