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
#include "app_command.h"
#include "bsp.h"
#include "command_dispatch.h"
#include "modem_component.h"
#include "store.h"
#include "svc_log.h"

#include <string.h>

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
    bool identityLogged;     /* 已就"设备身份(SN)不可用"打过告警，避免每拍刷屏 */
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
/* NOTE (2026-09-28 改为运行期取值)：周期上报的间隔**不再是编译期常量**——它现在是业务
 * 参数 `transintvl`（1~20 分钟，掉电保存在 EEPROM，可用 `AT+TRANSINTVL=<n>` 在线修改），
 * 因此原 `MODEM_REPORT_TICKS` 宏已由下面的 `reportIntervalTicks()` 取代。 */
#define MODEM_CELL_TICKS    MODEM_PERIOD_TICKS(APP_CFG_MODEM_CELL_REFRESH_PERIOD_S * 1000U)
/*! 断电重启的断电时长（Process 周期数）；上电后的等待复用"等模组启动"那一段。 */
#define MODEM_POWER_OFF_TICKS \
    MODEM_PERIOD_TICKS(APP_CFG_MODEM_POWER_OFF_MS)

/*--------------------------------------------------------------------------*/
/* 周期上报间隔（业务参数 `transintvl`，单位分钟） ---------------------------*/
/**
 * @brief 当前上报间隔（分钟）：取自参数影子，非法或越界时回落到出厂默认值。
 *
 * NOTE (为什么每个 Process 周期都读一次)：`transintvl` 的真相在参数影子（上电时被
 *   EEPROM 里的有效记录覆盖编译期默认值），而 `AT+TRANSINTVL=<n>` 只改影子就立刻返回。
 *   AO 在这里每拍读一次，就自然做到「改完立即生效、不必重启」；代价只是一次数组下标
 *   加一次短字符串解析，**不碰 I2C 总线**，放在 100 ms 周期里毫无压力。
 * NOTE (为什么还要夹一次范围)：介质里可能写着旧固件的值（那时上限是 1440），也可能被
 *   外部写坏。这里兜底，保证换算出的节奏永远是 1~20 分钟。
 */
static uint32_t reportIntervalMin(void) {
    uint32_t minutes = 0U;

    if (!SVC_Param_TextToU32(App_Store_Value(SVC_PARAM_TRANSINTVL), &minutes) ||
        (minutes < (uint32_t)APP_CFG_MODEM_TRANS_INTERVAL_MIN) ||
        (minutes > (uint32_t)APP_CFG_MODEM_TRANS_INTERVAL_MAX)) {
        return (uint32_t)APP_CFG_MODEM_TRANS_INTERVAL_DEFAULT;
    }
    return minutes;
}

/** @brief 上报间隔换算成 Process 周期数（至少 1 拍：算出 0 会变成每拍都上报）。 */
static uint32_t reportIntervalTicks(void) {
    uint32_t ticks = MODEM_PERIOD_TICKS(reportIntervalMin() * 60000U);

    return (ticks == 0U) ? 1U : ticks;
}

/** @brief 把可移植组件 trace 适配到现有异步日志。 */
static void componentTrace(void *context, char const *text) {
    (void)context;
    LOG_D("4g-core: %s", text);
}

/** @brief 设备身份（参数 `sn`）是否可用于拼 MQTT `clientId` 与主题。
*
* NOTE (为什么要有这道闸，2026-09-28)：`deviceId` 从编译期宏改成了**运行期取参数 `sn`**
*   （串口刷写、掉电保存），代价是"值可能不合法"。非法值的后果是**静默**的：
*   `sensor-product//command` 在 MQTT 里是合法主题，模组订阅照样回 `+MQTTSUBACK`，
*   只是永远收不到平台的消息——与 `08-改动日志` 里 `#51` 踩的是同一类坑。
*   所以宁可不启动 4G，也要把原因明确打到串口上。
*
* NOTE (判据在 Service 层)：`SVC_Param_CheckDeviceId()` 与命令层写参数时用的是
*   同一个函数，避免"写入放行、启动拒绝"这种两边不一致的偏差。
*/
static bool identityUsable(void) {
    return SVC_Param_CheckDeviceId(App_Store_Value(SVC_PARAM_BLE_SN),
                                   (uint8_t *)0) == SVC_DEVICE_ID_OK;
}

/** @brief 身份不可用时每个周期检查一次，但**只告警一次**（避免刷屏）。
*
* NOTE (日志必须带原因，2026-09-28 当天补)：判据上线当天就被自己坑了一次——只打
*   "unusable" 看不出到底哪里不合法。现在带原因码、出错下标与那一个字节，
*   **仍然不打印值本身**（它决定 MQTT 主题，进日志等于把设备身份写进串口历史）。 */
static void noteIdentityUnusable(Modem *me) {
    if (!me->identityLogged) {
        char const *sn = App_Store_Value(SVC_PARAM_BLE_SN);
        uint8_t badIndex = 0U;
        SvcDeviceIdStatus const status = SVC_Param_CheckDeviceId(sn, &badIndex);

        me->identityLogged = true;
        LOG_E("modem: device SN unusable reason=%u pos=%u ch=0x%02X len=%u "
              "-> 4G disabled; provision it: AT+SN=\"<device-id>\"",
              (unsigned)status,
              (unsigned)badIndex,
              (unsigned)(uint8_t)sn[badIndex],
              (unsigned)strlen(sn));
    }
}

/** @brief 连接失败时打印一次诊断读数。 */
static void logFailure(Modem *me) {
    BspModemDiag diag;

    BSP_modemUartDiag(&diag);
    /* NOTE (寄存器快照，2026-09-30 加)：`rx=0` 光看计数**分不清四种原因**，现场为此在示波器上
    *   耗了很久。把外设状态一并打出来即可当场分开：
    *     ① `cr1` 的位 5（`RXNEIE`）= 0 → **接收根本没武装**（软件）；
    *     ② `nvic` = 0                  → **中断没使能**（软件）；
    *     ③ `brr` 不是该速率的分频值      → **波特率没写进外设**（72 MHz 下 9600 = `0x1D4C`）；
    *     ④ 以上都正常而 rx 仍为 0        → **引脚上确实没有信号**（回到硬件查电平与接线）。
    *   读寄存器的动作在 BSP 层（`BSP_modemUartDiag()`）：App 层不得直接接触寄存器（AGENTS 5.1）。 */
    /* NOTE (rx 字段，2026-09-27 加)：uart_err 只能说明「收到了畸形字节」，
    *   区分不了「线上一片安静」和「对方根本不应答」——而波特率配错正好属于后者
    *   （模组听不懂就不会回，我们这边也收不到任何字节、uart_err 一直是 0）。
    *   加上 rx（累计收到字节数）后：rx=0 且 uart_err=0 = 线上一片安静；
    *   rx>0 且 uart_err>0 = 字节来了但成帧失败，优先怀疑波特率。 */
    LOG_W("modem: link failed reason=%u step=%u rx=%lu tx_ovf=%lu uart_err=%lu cr1=0x%04lX sr=0x%04lX brr=0x%04lX nvic=%u",
          (unsigned)me->component.failure,
          /* NOTE (step 口径，2026-09-27)：`failure_step` 存的是**从 0 开始**的计划下标，
           *   而 trace 打的是 `step k/N`（从 1 开始）。直接印会出现「trace 说第 18 步失败、
           *   这行却说 step=17」的矛盾，现场要核对半天。这里 +1 与 trace 对齐。 */
          (unsigned)me->component.failure_step + 1U,
          (unsigned long)BSP_modemUartRxCount(),
          (unsigned long)BSP_modemOverflowCount(),
          (unsigned long)BSP_modemErrorCount(),
          (unsigned long)diag.cr1,
          (unsigned long)diag.sr,
          (unsigned long)diag.brr,
          (unsigned)diag.nvic);
}

/** @brief 进入在线态时打印一次。 */
static void logReady(Modem *me) {
    LOG_I("modem: cloud link ready, report every %lu s",
          (unsigned long)(reportIntervalMin() * 60U));
    LOG_I("modem: signal=%ld cell_tac=%s cell_id=%s",
          (long)me->component.data.signal_quality,
          (me->component.data.tac[0] != '\0') ? me->component.data.tac : "-",
          (me->component.data.cell_id[0] != '\0') ? me->component.data.cell_id : "-");
}

/** @brief 执行一条来自云端的 AT 命令，并把结果交回组件发布。
*
* 这是「串口与 4G 共用同一套命令语义」的 4G 侧落点：命令文本**不另写解析**，
* 而是逐字节喂进 Service 层的 `SVC_At_Feed()`，拿到 `SvcAtCmd` 后走同一个
* `App_Command_Dispatch()`。因此 4G 与串口对 `AT+SN?` 的理解、错误码、响应文本
* 完全一致，新增命令只需要改 `command_dispatch.c` 一处。
*
* NOTE (不做就地等待)：本函数只跑一次「解析 + 分发」，两个动作都不访问总线
*   （读参数走 RAM 影子，写参数只置 dirty 由 AO_Store 异步落盘），所以不会拖住 QV。
*
* NOTE (失败文本，平台规则 3.4)：平台只认以最终 `ERROR\r\n` 结尾的失败回复，而本设备
*   的失败形态是 `+CME ERROR:<n>\r\n`（V.250 里它本身就是最终结果码）。两者不冲突：
*   规则 4.2 允许「可选错误行 + ERROR」，所以这里把 `+CME ERROR:<n>` 当详细行、
*   再补一行 `ERROR` 收尾。**这一步只发生在 4G 回包上，串口输出保持原样。**
*/
static void serveDownlinkCommand(Modem *me) {
    char id[APP_CFG_MODEM_DOWNLINK_ID_SIZE];
    char text[APP_CFG_MODEM_DOWNLINK_COMMAND_SIZE];
    char raw[SVC_AT_RESP_MAX + 8U];
    SvcAt at;
    SvcAtCmd cmd;
    AppCommandRequest request;
    AppCommandResult result;
    size_t index;
    size_t length;
    bool parsed;
    bool ok;

    if (!Modem_Component_TakeCommand(&me->component, id, sizeof(id),
                                     text, sizeof(text))) {
        return;
    }

    /* 与串口同一条路：先逐字节累积，再用一个行结束符触发解析。
     * 平台规则第 1 条保证 `command` 里不带 CR/LF，所以这里自己补 '\r'。 */
    SVC_At_Init(&at);
    for (index = 0U; (text[index] != '\0') && (index < sizeof(text)); ++index) {
        (void)SVC_At_Feed(&at, (uint8_t)text[index], &cmd);
    }
    parsed = SVC_At_Feed(&at, (uint8_t)'\r', &cmd);

    if (!parsed) {
        /* 认不出来的命令（拼错、缺 `AT` 前缀、超长被行缓冲丢弃）：回失败应答。
         * NOTE (凭据纪律，AGENTS.md 4.2)：这里**只打关联 ID，不打命令原文**——
         *   云端命令可能是 `AT+PIN="…"`，原样打日志等于把凭据写进串口历史。 */
        LOG_W("modem: cloud command not recognised, id=%s", id);
        Modem_Component_CompleteCommand(&me->component, id, false, "ERROR\r\n");
        return;
    }

    (void)memset(&request, 0, sizeof(request));
    request.source = APP_COMMAND_SOURCE_MODEM;
    request.command = cmd;
    length = strlen(id);
    if (length >= sizeof(request.correlation)) {
        length = sizeof(request.correlation) - 1U;
    }
    (void)memcpy(request.correlation, id, length);
    request.correlation[length] = '\0';
    request.has_correlation = true;

    if (!App_Command_Dispatch(&request, &result) || (result.raw[0] == '\0')) {
        LOG_W("modem: cloud command reached no handler, id=%s", id);
        Modem_Component_CompleteCommand(&me->component, id, false, "ERROR\r\n");
        return;
    }

    ok = (result.status != APP_COMMAND_RESULT_ERROR);
    if (ok) {
        /* 成功也要留一行：否则「设备到底执行没执行」只能靠平台那侧看，排障时少一半证据。
         * NOTE (凭据纪律)：只打关联 ID，不打命令原文、不打响应原文。 */
        LOG_I("modem: cloud command done, id=%s", id);
        Modem_Component_CompleteCommand(&me->component, id, true, result.raw);
        return;
    }

    /* 失败：先原样带上设备自己的错误行，再补最终 ERROR（原本已是 ERROR 时不重复） */
    (void)memcpy(raw, result.raw, sizeof(result.raw));
    if (strstr(raw, "ERROR\r\n") == (char *)0) {
        (void)strncat(raw, "ERROR\r\n", sizeof(raw) - strlen(raw) - 1U);
    }
    LOG_W("modem: cloud command failed, id=%s err=%u", id,
          (unsigned)result.error_code);
    Modem_Component_CompleteCommand(&me->component, id, false, raw);
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
                /* NOTE (必须等参数就绪，2026-09-28)：`deviceId` 取自参数 `sn`，而 EEPROM 里的
                 *   真实值由 AO_Store 异步读回——上电瞬间影子里还是编译期默认值。
                 *   不等就启动，会把 10 条主题全拼成默认 SN，而且**模组订阅会成功**
                 *   （只是订了个平台不会发的主题），现场表现为「设备在线、命令没反应」。
                 *   同一个坑在本工程的 BLE 侧已经踩过，那里也是等 `App_Store_ConfigResolved()`。 */
                if (!App_Store_ConfigResolved()) {
                    return Q_HANDLED();
                }
                /* 身份不可用（未刷 SN / 含非法字符）→ 拒绝启动 4G，并给出一次明确告警。
                 * 产线刷完 SN 会触发自动重启，重启后自然就绪；这里每个周期重判，
                 * 所以也不怕"运行期把 SN 改好"——那种情况本来就会重启。 */
                if (!identityUsable()) {
                    noteIdentityUnusable(me);
                    return Q_HANDLED();
                }
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
            /* 云端下行的 AT 命令（平台规则 3.2）在这里执行：组件只拆包，
             * 解析与分发都在本层，见 serveDownlinkCommand()。 */
            serveDownlinkCommand(me);

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
                /* 把存储里的业务参数同步进组件的只读镜像：旧云属性通道 `sample_interval`
                 * 自 2026-09-28 起**不再驱动**上报周期（真节奏见 reportIntervalMin()），
                 * 但对它的**读取**应当与设备实际节奏一致——否则平台会读到 1 分钟，
                 * 而设备其实每 20 分钟才发一次。写它不会改变节奏（下一段代码说了算）。 */
                me->component.sample_interval_min = reportIntervalMin();
                logReady(me);
                /* NOTE (连上后立刻上报一帧，2026-09-29)：以前第一帧要等满一个上报周期
                 *   （`TRANSINTVL` 出厂默认 20 分钟），现场看到的是「4G 明明连上了，平台
                 *   却半天没有数据」——分不清是链路没通、还是周期没到。上电先报一次当前值：
                 *   平台立刻有卡片与折线，也让「数据是不是新的」有了明确起点。
                 * NOTE (把计数归零)：保证下一帧仍从此刻起算一个**完整**周期，这次额外汇报
                 *   不会把后续节奏提前。
                 * NOTE (尊重停报开关)：平台用可写属性 `report_enabled` 临时停报时，
                 *   这里同样不发——否则「停报」在上电那一刻会被绕过。 */
                if (me->component.report_enabled) {
                    me->reportTicks = 0U;
                    Modem_Component_RequestReport(&me->component);
                }
            }
            /* 周期上报与小区信息刷新：只置请求位，真正的 AT 往返由组件在
             * 下一个 Process 周期发起，AO 不等待。间隔每拍从参数影子取一次，
             * 因此 `AT+TRANSINTVL=<n>` 改完立即生效（改小后下一拍就可能立即上报）。 */
            if (me->component.report_enabled &&
                (++me->reportTicks >= reportIntervalTicks())) {
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
            serveDownlinkCommand(me);
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

bool Modem_IsUplinkIdle(void) {
    /* NOTE (只看这两个字段)：`reply_pending` = 已排队但还没发出去的命令应答；
     *   `runtime_op != NONE` = 有一条上行命令正在等模组应答。
     *   两者都空 = 该发的都发出去了，可以安全复位。
     *   本函数只读、不加锁：QV 单栈协作，AO_System 读它时 AO_Modem 一定不在运行
     *   （状态处理函数运行到完成才让出），因此不会读到半个状态。 */
    return (!l_modem.component.reply_pending) &&
           (l_modem.component.runtime_op == MODEM_RUNTIME_NONE);
}

void Modem_ctor(void) {
    Modem *me = &l_modem;

    QActive_ctor(&me->super, Q_STATE_CAST(&initial));
    QTimeEvt_ctorX(&me->processEvt, &me->super, MODEM_PROCESS_SIG, 0U);
    /* NOTE (凭据)：这里只把凭据交给组件，绝不写进任何日志。 */
    me->config = (ModemConfig){APP_CFG_MODEM_HOST, APP_CFG_MODEM_PORT,
                               APP_CFG_MODEM_PRODUCT_ID,
                               App_Store_Value(SVC_PARAM_BLE_SN),
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
    me->identityLogged = false;
    BSP_modemUartInit(&me->port, (bsp_uart_id_t)APP_CFG_MODEM_UART_ID);
    me->port.trace = &componentTrace;
    Modem_Component_Init(&me->component, &me->port, &me->config);
}

void Modem_start(void) {
    QACTIVE_START(AO_Modem, APP_MODEM_PRIO, l_queue, Q_DIM(l_queue),
                  (void *)0, 0U, (QEvt *)0);
}

#endif /* APP_CFG_MODEM_ENABLE != 0U */
