/*============================================================================
* modem_mn316.c —— MN316 后端：中移芯昇 NB-IoT 模组的 AT 与 MQTT 指令集
* 层次定位：Component 层 backend；只被 modem_component.c 通过 ModemBackend 调用。
*
* 模型特点：直连式 MQTT——每次发布直接给出主题字符串，负载必须是十六进制文本；
*   下行通知 +MQTTPUBLISH: 自带负载字节数，因此完整性判定按声明长度而不是换行。
*
* 来源：由 E:\stm32\stm32\Drivers\Modem\Src\backend\mn316_backend.c（阻塞版）改写：
*   - 原来的「发命令 → 就地轮询等应答」拆成一条线性步骤表，每步只负责组装命令；
*   - 等待、重试与超时都由核心按期限推进，后端里没有任何循环等待；
*   - 后端不再持有静态状态，全部状态放在 ModemComponent 里。
*
* 未选中本后端时整个文件不参与编译（见文件末尾的 #if）。
*===========================================================================*/
#include "modem_backend.h"
#include <stdio.h>
#include <string.h>

#if (MODEM_COMPONENT_BACKEND == MODEM_BACKEND_MN316)

/** 模组 MQTT 操作超时参数（秒），对应 AT+MQTTTO。 */
#define MN316_MQTT_TIMEOUT_S  (20U)

/** 射频复位两阶段之间的等待，以及发完命令后留给模组的静默时间（毫秒）。 */
#define MN316_RECOVER_OFF_MS     (3000U)
#define MN316_RECOVER_SETTLE_MS  (5000U)

/** APN 名字的最大长度（含结尾 '\0'）。3GPP 里 APN 最长 100 字符，
 *  常见值（cmnet / ctnb / 定向卡专用名）都在 24 以内；这里够用且省栈。 */
#define MN316_APN_MAX            (24U)

/*--------------------------------------------------------------- 计划表 */
/*! 连接计划：按顺序执行；TOPIC 步由核心按 topic_cursor 逐条展开。
*  NOTE (配置)：所有时间与次数都来自 app_config.h 的 APP_CFG_MODEM_*，
*  本表里不写裸数字，调整节奏只改配置。 */
static ModemStep const kMn316Plan[] = {
    /* op                     mode                  timeout                            retry                            budget                             attempts                          tolerate */
    {MODEM_OP_PROBE,          MODEM_STEP_REPEAT,    MODEM_COMPONENT_PROBE_TIMEOUT_MS,   MODEM_COMPONENT_AT_RETRY_MS,     MODEM_COMPONENT_PROBE_BUDGET_MS,   MODEM_COMPONENT_PROBE_ATTEMPTS,   (char const *)0},
    {MODEM_OP_ECHO_OFF,       MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    {MODEM_OP_CMEE,           MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    {MODEM_OP_RADIO,          MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    {MODEM_OP_SIM,            MODEM_STEP_REPEAT,    MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_POLL_INTERVAL_MS, MODEM_COMPONENT_SIM_WAIT_MS,      1U,                               (char const *)0},
    {MODEM_OP_IDENTITY,       MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    {MODEM_OP_SIGNAL,         MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    {MODEM_OP_SIGNAL_EXT,     MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    {MODEM_OP_OPERATOR,       MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_CONFIG_TIMEOUT_MS,  MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    {MODEM_OP_REG_MODE,       MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    {MODEM_OP_REGISTRATION,   MODEM_STEP_REPEAT,    MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_REG_POLL_MS,     MODEM_COMPONENT_REG_TIMEOUT_MS,    1U,                               (char const *)0},
    {MODEM_OP_ATTACHED,       MODEM_STEP_REPEAT,    MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_POLL_INTERVAL_MS, MODEM_COMPONENT_ATTACH_TIMEOUT_MS, 1U,                               (char const *)0},
    {MODEM_OP_IP_ADDRESS,     MODEM_STEP_ONCE,      MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    /* NOTE (PDP/APN 诊断步，2026-09-27)：只读地把模组当前配置的 APN 打到 trace 里，
     *   用来判读第 18 步「连不上 MQTT 服务器」到底是网络侧哪种情况。动机见下方
     *   mn316Build 的 MODEM_OP_PDP 分支。
     *   ⚠ **这一步是插在计划表中间的，它把后面所有步骤的编号 +1**：MN316 由 21 步变
     *   **22 步**，`AT+MQTTCFG` 由第 17 步变第 18 步、`AT+MQTTOPEN` 由第 18 步变第 19 步。
     *   改动日志与 `App/component/README.md` 里引用的旧编号属历史记录，不回改；
     *   本文件里凡是按编号写的注释都已同步。 */
    {MODEM_OP_PDP,            MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    {MODEM_OP_MQTT_DISC,      MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_CONFIG_TIMEOUT_MS,  MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    /* NOTE (tolerate)：无客户端时本模组对 AT+MQTTDEL 必回 +CME ERROR:8002，
     *   而「没有客户端」正是本步要的结果，故声明为可接受（见 modem_backend.h 的字段说明）。 */
    {MODEM_OP_MQTT_DELETE,    MODEM_STEP_REPEAT,    MODEM_COMPONENT_CONFIG_TIMEOUT_MS,  MODEM_COMPONENT_AT_RETRY_MS,     0U,                                2U,                               "+CME ERROR:8002"},
    {MODEM_OP_MQTT_TIMEOUT,   MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS,  MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    /* NOTE (800x 重试预算，2026-09-27)：上板实测第 17 步 `AT+MQTTCFG=` 会回
     *   `+CME ERROR:800x`。手册（MN316 MQTT 用户手册 V1.0.0 §2.1，第 8~9 页）确认
     *   **命令形态本身没错**：8 个参数 host/port/id/keepAlive/user/passwd/clean/encrypt，
     *   且 clean=1、encrypt=0 都是合法取值；参数语法错会回 `+CME ERROR:4`（本仓库
     *   在 ML307C 上就见过那个码），不是 800x。所以 800x 属于**模组状态类**错误码。
     *
     *   手册 §3.3（第 22 页）给的流程是 `AT+MQTTDISC` → `AT+MQTTDEL`
     *   （注释原文：「释放资源，之后可以再次使用 +MQTTCFG 进行配置」）→ 再 `AT+MQTTCFG`，
     *   与本计划表的顺序一致，**顺序也没错**。剩下的解释就是：刚释放完资源时模组还没准备好
     *   接受重新配置，需要一个更长的重试窗口——这正是本仓库在 #11 的遗留里就写下的预判：
     *   「若实机出现 `+CME ERROR:8002` 需把 `MODEM_OP_MQTT_CONFIG` 的 `attempts` 调大」。
     *   故次数 2 → 6、重发间隔由 500 ms（AT_RETRY）改为 1 s（POLL_INTERVAL），
     *   合计约 6 秒的重试窗口。
     *
     *   NOTE (为什么不是 tolerate)：把 800x 当成功会让紧随其后的 `AT+MQTTOPEN` 在
     *   「没有配置好的客户端」的情况下失败，只是把故障点往后推、更难定位。要**重试**，
     *   不要容忍——这与 `MODEM_OP_MQTT_DELETE` 那条的语义正好相反。 */
    {MODEM_OP_MQTT_CONFIG,    MODEM_STEP_REPEAT,    MODEM_COMPONENT_CONFIG_TIMEOUT_MS,  MODEM_COMPONENT_POLL_INTERVAL_MS, 0U,                              6U,                               (char const *)0},
    {MODEM_OP_MQTT_OPEN,      MODEM_STEP_REPEAT,    MODEM_COMPONENT_MQTT_OPEN_TIMEOUT_MS, MODEM_COMPONENT_POLL_INTERVAL_MS, 0U,                              1U,                               (char const *)0},
    {MODEM_OP_MQTT_STATE,     MODEM_STEP_REPEAT,    MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_POLL_INTERVAL_MS, MODEM_COMPONENT_MQTT_STATE_TIMEOUT_MS, 1U,                          (char const *)0},
    {MODEM_OP_TOPIC_SUB,      MODEM_STEP_TOPIC,     MODEM_COMPONENT_TOPIC_TIMEOUT_MS,   MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0},
    /* NOTE (补查信号，2026-09-20)：第 7 步的 AT+CSQ 跑在注册之前，模组必然回 99（未知），
     *   于是上报里的 signal_quality 永远是 99。这里在 MQTT 上线后再查一次，让它有真值。
     *   NOTE (为什么追加在末尾)：计划表按下标取值、后端按 op 分派，追加一行只多一步，
     *   不会改变已有步骤的编号（避免与日志/文档里的 step k/N 对不上）。 */
    {MODEM_OP_SIGNAL,         MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,         MODEM_COMPONENT_AT_RETRY_MS,     0U,                                1U,                               (char const *)0}
};

/*--------------------------------------------------------------- 小工具 */
/** @brief 把期望应答标记写入调用方缓冲。 */
static bool setExpected(char *expected, size_t capacity, char const *token) {
    size_t length = strlen(token);

    if ((expected == (char *)0) || ((length + 1U) > capacity)) {
        return false;
    }
    (void)memcpy(expected, token, length + 1U);
    return true;
}

/** @brief 检查 snprintf 是否把命令完整写进了组件缓冲。 */
static bool commandReady(ModemComponent *me, int produced) {
    return (produced >= 0) && ((size_t)produced < sizeof(me->command));
}

/** @brief 在应答文本里找标记，返回标记之后的位置。 */
static char const *afterMarker(char const *text, char const *marker) {
    char const *position = strstr(text, marker);
    return (position == (char const *)0) ? (char const *)0 : position + strlen(marker);
}

/** APN 解析结果。NOTE (为什么要分三种，2026-09-27)：上板打出的第一版只有
 *  「parsed / unparsed」两态，而 `unparsed` 同时覆盖了「上下文里 APN 为空」
 *  与「根本没有 +CGDCONT 行」这两种**含义完全不同**的情况，等于没诊断。 */
typedef enum {
    MN316_APN_OK = 0,   /*!< 取到非空 APN                      */
    MN316_APN_EMPTY,    /*!< 有这一行，但 APN 字段为空串        */
    MN316_APN_MISSING   /*!< 找不到该行 / 字段不足              */
} mn316ApnResult;

/** @brief 取 `+<marker>:` 之后的第 ordinal 个引号字段（ordinal 从 1 起）。
*
* NOTE (为什么不用 `ModemAt_NextQuoted()`)：它对**空字段**返回 false
*   （`modem_at.c:169` 的 `length == 0` 分支），而「字段为空」恰恰是最需要
*   单独区分出来的一种结果，不能和「压根没有这个字段」混为一谈。这里自己走引号，
*   把空字段当成合法值（返回 true 且 out 为空串）。
*
* @return true = 该字段存在（**可能为空串**）；false = 找不到
*/
static bool mn316ReadQuotedField(char const *text, char const *marker, uint8_t ordinal,
                                 char *out, size_t capacity) {
    char const *cursor;
    char const *start;
    char const *end;
    size_t length;
    uint8_t found = 0U;

    if ((out == (char *)0) || (capacity == 0U)) {
        return false;
    }
    out[0] = '\0';

    cursor = afterMarker(text, marker);
    if (cursor == (char const *)0) {
        return false;
    }
    while (*cursor != '\0') {
        start = strchr(cursor, '"');
        if (start == (char const *)0) {
            return false;
        }
        end = strchr(start + 1, '"');
        if (end == (char const *)0) {
            return false;
        }
        ++found;
        if (found == ordinal) {
            length = (size_t)(end - (start + 1));
            if (length >= capacity) {
                length = capacity - 1U;
            }
            (void)memcpy(out, start + 1, length);
            out[length] = '\0';
            return true;
        }
        cursor = end + 1;
    }
    return false;
}

/** @brief 从 `+CGCONTRDP:` 行取「实际在用」的 APN（第 1 个引号字段）。
*
* 字段顺序（3GPP 27.007）：`<cid>,<bearer_id>,"<apn>","<local_addr>","<gateway>",...`
* —— 与 `+CGDCONT` 不同，**第 1 个引号字段就是 APN**（`+CGDCONT` 的第 1 个是 PDP 类型）。
*/
static mn316ApnResult mn316ReadActiveApn(char const *text, char *out, size_t capacity) {
    char const *cursor = afterMarker(text, "+CGCONTRDP:");
    char field[MN316_APN_MAX];

    if (cursor == (char const *)0) {
        return MN316_APN_MISSING;
    }
    if (!mn316ReadQuotedField(text, "+CGCONTRDP:", 1U, field, sizeof(field))) {
        return MN316_APN_MISSING;
    }
    if (field[0] == '\0') {
        return MN316_APN_EMPTY;
    }
    (void)snprintf(out, capacity, "%s", field);
    return MN316_APN_OK;
}

/** @brief 把应答压成一行可打印 ASCII，供诊断步骤在解析不出来时打出。
*
* NOTE (为什么要它，2026-09-27)：`MODEM_OP_PDP` 第一版解析失败时只打 `unparsed`，
*   结果上板看到的就是这四个字——**仍然要猜模组回了什么**。诊断步骤的意义就是把未知量
*   变成可读事实，所以必须带出原文。组件的 `describeReply()` 是它内部函数、后端看不到，
*   这里自己清洗（逻辑很简单）。
* NOTE (为什么现在连「解析成功」也要打)：上板拿到 `pdp apn="ctnb.MNC011.MCC460.GPRS" gw=none`
*   ——APN 取到了，但网关取不到。这可能是真的没有网关，也可能是**模组把网关给成了不带引号
*   的字段**（3GPP 规定是带引号的字符串，但实现有差异），而本函数只认引号字段。这个区别
*   直接决定结论，所以把 `+CGCONTRDP:` 那一行**原样**打出来，不再靠猜。
* @param marker 起点标记（例如 `"+CGCONTRDP:"`）；传 NULL 表示从头开始 */
static void mn316FormatRaw(char const *text, char const *marker, char *out, size_t capacity) {
    static char const prefix[] = "pdp raw=\"";
    char const *start = text;
    size_t used = 0U;
    size_t index = 0U;

    if ((out == (char *)0) || (capacity < 4U)) {
        return;
    }
    if (marker != (char const *)0) {
        char const *found = afterMarker(text, marker);
        if (found != (char const *)0) {
            start = found;
        }
    }
    while ((prefix[used] != '\0') && ((used + 1U) < capacity)) {
        out[used] = prefix[used];
        ++used;
    }
    /* NOTE (长度预算)：App 侧日志的格式化缓冲是 APP_CFG_LOG_FMT_BUF_SIZE（96），
     * 前缀 "4g-core: " 占 9，所以正文控制在 64 字符以内，避免被截断在半路上。 */
    while ((start != (char const *)0) && (start[index] != '\0') &&
           (index < 64U) && ((used + 2U) < capacity)) {
        unsigned char const character = (unsigned char)start[index];
        out[used] = ((character >= 0x20U) && (character < 0x7FU)) ? (char)character : '|';
        ++used;
        ++index;
    }
    if ((used + 1U) < capacity) {
        out[used] = '"';
        ++used;
    }
    out[used] = '\0';
}

/*--------------------------------------------------------------- 命令组装 */
/** @brief 组装连接计划第 step 步的命令。 */
static bool mn316Build(ModemComponent *me, uint8_t step, char *expected,
                       size_t expectedSize) {
    int produced = -1;

    switch (kMn316Plan[step].op) {
        case MODEM_OP_PROBE:
            produced = snprintf(me->command, sizeof(me->command), "AT");
            break;
        case MODEM_OP_ECHO_OFF:
            produced = snprintf(me->command, sizeof(me->command), "ATE0");
            break;
        case MODEM_OP_CMEE:
            produced = snprintf(me->command, sizeof(me->command), "AT+CMEE=1");
            break;
        case MODEM_OP_RADIO:
            produced = snprintf(me->command, sizeof(me->command), "AT+CFUN?");
            break;
        case MODEM_OP_SIM:
            produced = snprintf(me->command, sizeof(me->command), "AT+CIMI");
            break;
        case MODEM_OP_IDENTITY:
            produced = snprintf(me->command, sizeof(me->command), "AT+CGSN=1");
            break;
        case MODEM_OP_SIGNAL:
            produced = snprintf(me->command, sizeof(me->command), "AT+CSQ");
            break;
        case MODEM_OP_SIGNAL_EXT:
            produced = snprintf(me->command, sizeof(me->command), "AT+CESQ");
            break;
        case MODEM_OP_OPERATOR:
            produced = snprintf(me->command, sizeof(me->command), "AT+COPS?");
            break;
        case MODEM_OP_REG_MODE:
            /* 结果码 2：注册状态变化时同时上报位置信息，后续即可取到 TAC 与小区标识。 */
            produced = snprintf(me->command, sizeof(me->command), "AT+CEREG=2");
            break;
        case MODEM_OP_REGISTRATION:
            produced = snprintf(me->command, sizeof(me->command), "AT+CEREG?");
            break;
        case MODEM_OP_ATTACHED:
            produced = snprintf(me->command, sizeof(me->command), "AT+CGATT?");
            break;
        case MODEM_OP_IP_ADDRESS:
            produced = snprintf(me->command, sizeof(me->command), "AT+CGPADDR=0");
            break;
        case MODEM_OP_PDP:
            /* NOTE (为什么加这一步，2026-09-27)：上板实测模组能注册、能附着、
             *   能拿到 IP（`AT+CGPADDR=0` 通过），却连不上 MQTT 服务器——
             *   `AT+MQTTOPEN` 回 `OK` 之后收到 `+MQTTOPEN:FAIL`（手册 §2.2：即
             *   「连接服务器失败」）。而同一端口从 PC 上 `Test-NetConnection` 是通的，
             *   域名与纯 IP 都试过、结果一样（DNS 已排除）。于是问题被夹在
             *   「模组 → 服务器」这一跳，最大嫌疑是 **APN**：本计划表从头到尾
             *   **没有任何 APN 配置**，完全吃模组默认值。
             *
             *   NOTE (为什么从 `AT+CGDCONT?` 换成 `AT+CGCONTRDP=0`，2026-09-27 第二步)：
             *   `AT+CGDCONT?` 已经问过了，答复是 **APN 字段为空**
             *   （上板日志 `pdp apn EMPTY (context exists, APN field blank)`）——
             *   它只说明「没有**静态配置**的 APN」，**回答不了「网络实际在用哪个 APN」**。
             *   而 `AT+CGCONTRDP=0` 回的是**已激活上下文**的参数：
             *   `<cid>,<bearer_id>,"<apn>","<local_addr>","<gateway>","<dns1>","<dns2>"`，
             *   一次给出「实际 APN + 本地 IP + 网关 + DNS」——APN 是公网名还是定向专网名，
             *   以及「有 IP 却没网关（= 没有到公网的路由）」这一少见情况，都能当场判读。
             *   仍然是只读、`OPTIONAL`：本模组若不支持该命令就是回 `ERROR`，
             *   该步被容忍跳过，不影响连接流程（届时日志里连 `pdp` 这一行都不会有）。 */
            produced = snprintf(me->command, sizeof(me->command), "AT+CGCONTRDP=0");
            break;
        case MODEM_OP_MQTT_DISC:
            produced = snprintf(me->command, sizeof(me->command), "AT+MQTTDISC");
            break;
        case MODEM_OP_MQTT_DELETE:
            /* NOTE (2026-09-20 修正判据)：本模组在**没有 MQTT 客户端**时对 AT+MQTTDEL
             *   一律回 `+CME ERROR:8002`（#13 的 PC 实测结论），这是稳定状态而不是
             *   瞬时状态——原注释以为「断开还没完成」、靠重试等它消失，实测等不到。
             *   而「没有客户端」正是一开始想要的状态，所以由计划表的 tolerate 声明
             *   8002 为可接受结果（见下面 kMn316Plan 的本行）。 */
            produced = snprintf(me->command, sizeof(me->command), "AT+MQTTDEL");
            break;
        case MODEM_OP_MQTT_TIMEOUT:
            produced = snprintf(me->command, sizeof(me->command), "AT+MQTTTO=%u",
                                (unsigned)MN316_MQTT_TIMEOUT_S);
            break;
        case MODEM_OP_MQTT_CONFIG:
            /* NOTE (凭据)：用户名与密码只写进 AT 命令，绝不写进任何日志。 */
            if ((!ModemAt_IsSafeText(me->config->host)) ||
                (!ModemAt_IsSafeText(me->config->device_id)) ||
                (!ModemAt_IsSafeText(me->config->username)) ||
                (!ModemAt_IsSafeText(me->config->password))) {
                return false;
            }
            produced = snprintf(me->command, sizeof(me->command),
                                "AT+MQTTCFG=\"%s\",%u,\"%s\",%u,\"%s\",\"%s\",%u,0",
                                me->config->host, (unsigned)me->config->port,
                                me->config->device_id,
                                (unsigned)me->config->keep_alive_s,
                                me->config->username, me->config->password, 1U);
            break;
        case MODEM_OP_MQTT_OPEN:
            /* 打开成功时会回一条 "MQTTOPEN:OK" 的结果码，而不是普通的 OK。 */
            produced = snprintf(me->command, sizeof(me->command),
                                "AT+MQTTOPEN=1,1,0,0,0,\"\",\"\"");
            if (!commandReady(me, produced)) {
                return false;
            }
            return setExpected(expected, expectedSize, "+MQTTOPEN:OK");
        case MODEM_OP_MQTT_STATE:
            produced = snprintf(me->command, sizeof(me->command), "AT+MQTTSTAT?");
            break;
        case MODEM_OP_TOPIC_SUB:
            while (me->topic_cursor < (uint8_t)MODEM_TOPIC_COUNT) {
                ModemTopicSlot const *slot = &me->topic[me->topic_cursor];
                if (slot->registered && (slot->subscribe_id != 0U)) {
                    produced = snprintf(me->command, sizeof(me->command),
                                        "AT+MQTTSUB=\"%s\",%u,0", slot->name,
                                        (unsigned)slot->publish_qos);
                    break;
                }
                ++me->topic_cursor;   /* 该主题不需要订阅：跳过，不影响后续 */
            }
            if (me->topic_cursor >= (uint8_t)MODEM_TOPIC_COUNT) {
                return false;         /* 没有可下发的主题，核心会推进整步 */
            }
            /* 订阅要等模组的订阅确认，而不是普通 OK。 */
            if (!commandReady(me, produced)) {
                return false;
            }
            return setExpected(expected, expectedSize, "+MQTTSUBACK");
        default:
            return false;
    }
    if (!commandReady(me, produced)) {
        return false;
    }
    return setExpected(expected, expectedSize, "OK");
}

/*--------------------------------------------------------------- 应答解析 */
/** @brief 解析第 step 步的应答；返回 false 表示这一步其实没通过。 */
static bool mn316Apply(ModemComponent *me, uint8_t step) {
    char const *cursor;
    char ipAddress[40];
    int32_t value = 0;

    switch (kMn316Plan[step].op) {
        case MODEM_OP_SIGNAL:
            cursor = afterMarker(me->response, "+CSQ:");
            if ((cursor != (char const *)0) && ModemAt_NextInt(&cursor, &value)) {
                me->data.signal_quality = value;   /* 99 表示未知，仍算查询成功 */
            }
            return true;
        case MODEM_OP_REGISTRATION: {
            /* 应答形如 +CEREG: <n>,<stat>[,"<tac>","<ci>",<AcT>]：有第二字段时以 <stat> 为准。 */
            int32_t first = 0;
            cursor = afterMarker(me->response, "+CEREG:");
            if ((cursor == (char const *)0) || (!ModemAt_NextInt(&cursor, &first))) {
                return false;
            }
            value = first;
            (void)ModemAt_NextInt(&cursor, &value);
            return (value == 1) || (value == 5);
        }
        case MODEM_OP_ATTACHED:
            cursor = afterMarker(me->response, "+CGATT:");
            if ((cursor == (char const *)0) || (!ModemAt_NextInt(&cursor, &value))) {
                return false;
            }
            return value == 1;
        case MODEM_OP_IP_ADDRESS:
            cursor = afterMarker(me->response, "+CGPADDR:");
            return (cursor != (char const *)0) &&
                   ModemAt_NextQuoted(&cursor, ipAddress, sizeof(ipAddress));
        case MODEM_OP_PDP:
            /* 只读诊断：把「实际在用」的 APN 与默认网关打到 trace 里。
             * **无论如何都返回 true**——这一步的目的是「看见」，不该因为模组没回或
             * 格式不同就拖垮整条计划。
             * NOTE (为什么 APN 可以打)：APN 是网络接入点名字，不是凭据；
             * 用户名/密码不在这条应答里。 */
            {
                char apn[MN316_APN_MAX];
                char gateway[24];
                char line[80];
                mn316ApnResult const result = mn316ReadActiveApn(me->response, apn, sizeof(apn));

                if (result == MN316_APN_OK) {
                    /* 顺带取默认网关：有 IP 却没有网关 = 根本没有到公网的路由，
                     * 这种少见但真实的情况只有靠它才能排除。 */
                    if (mn316ReadQuotedField(me->response, "+CGCONTRDP:", 3U,
                                             gateway, sizeof(gateway)) &&
                        (gateway[0] != '\0')) {
                        (void)snprintf(line, sizeof(line),
                                       "pdp apn=\"%s\" gw=\"%s\"", apn, gateway);
                    } else {
                        (void)snprintf(line, sizeof(line), "pdp apn=\"%s\" gw=none", apn);
                    }
                    MODEM_TRACE(me, line);
                    /* NOTE (为什么解析成功也打原文，2026-09-27)：上板拿到
                     *   `gw=none`，但「真没网关」与「模组把网关给成不带引号的字段、
                     *   而解析器只认引号字段」在日志上完全同形，而这两种情况的结论
                     *   天差地别（前者=没有公网路由，后者=解析器的问题）。
                     *   把 `+CGCONTRDP:` 那一行原样打出来，这个歧义就没了。 */
                    mn316FormatRaw(me->response, "+CGCONTRDP:", line, sizeof(line));
                    MODEM_TRACE(me, line);
                } else if (result == MN316_APN_EMPTY) {
                    (void)snprintf(line, sizeof(line),
                                   "pdp apn EMPTY (active context has blank APN)");
                    MODEM_TRACE(me, line);
                    mn316FormatRaw(me->response, "+CGCONTRDP:", line, sizeof(line));
                    MODEM_TRACE(me, line);
                } else {
                    mn316FormatRaw(me->response, "+CGCONTRDP:", line, sizeof(line));
                    MODEM_TRACE(me, line);
                }
            }
            return true;
        case MODEM_OP_MQTT_STATE:
            cursor = afterMarker(me->response, "+MQTTSTAT:");
            if ((cursor == (char const *)0) || (!ModemAt_NextInt(&cursor, &value))) {
                return false;
            }
            return value == 5;   /* 5 = 已连接 */
        default:
            return true;
    }
}

/*--------------------------------------------------------------- 主动通知 */
/** @brief 这一行是否像一条下行通知的开头。 */
static bool mn316UrcStarts(char const *text, size_t length) {
    (void)length;
    return strstr(text, "+MQTTPUBLISH:") != (char const *)0;
}

/** @brief 跳过前导字段，取出「负载字节数」与负载起点。
* @param declared [out] 模组声明的负载字节数
* @param payloadStart [out] 负载第一个字节的位置
* @return 通知头形态正确返回 true */
static bool mn316LocatePayload(char const *buffer, uint32_t *declared,
                               char const **payloadStart) {
    char const *cursor = strstr(buffer, "+MQTTPUBLISH:");
    char const *digits;

    if (cursor == (char const *)0) {
        return false;
    }
    cursor += strlen("+MQTTPUBLISH:");
    /* 通知格式：四个前导字段,主题,负载字节数,负载；按逗号切分头部。
     * 主题本身不能含逗号，这是本模组协议的限制。 */
    for (uint8_t count = 0U; count < 5U; ++count) {
        cursor = strchr(cursor, ',');
        if (cursor == (char const *)0) {
            return false;
        }
        ++cursor;
    }
    digits = cursor;
    *declared = 0U;
    while ((*digits >= '0') && (*digits <= '9')) {
        *declared = (*declared * 10U) + (uint32_t)(*digits - '0');
        ++digits;
    }
    if ((digits == cursor) || (*digits != ',')) {
        return false;
    }
    *payloadStart = digits + 1;
    return true;
}

/** @brief 通知是否已收全：按模组声明的负载字节数判断。 */
static bool mn316UrcComplete(char const *buffer, size_t length) {
    uint32_t declared = 0U;
    char const *payloadStart;
    size_t offset;

    if ((buffer == (char const *)0) || (length == 0U)) {
        return false;
    }
    if (!mn316LocatePayload(buffer, &declared, &payloadStart)) {
        return false;
    }
    offset = (size_t)(payloadStart - buffer);
    if (offset > length) {
        return false;
    }
    return (length - offset) >= (size_t)declared;
}

/** @brief 从 +MQTTPUBLISH 通知里取出逻辑主题与负载。 */
static bool mn316UrcExtract(ModemComponent *me, char const *text, size_t length,
                            ModemTopic *topic, char *payload, size_t payloadSize) {
    uint32_t declared = 0U;
    char const *payloadStart;
    char const *cursor;
    char const *topicStart = (char const *)0;
    char const *topicEnd = (char const *)0;
    size_t topicLength;
    char topicText[MODEM_COMPONENT_TOPIC_MAX];
    uint8_t index;

    if (!mn316LocatePayload(text, &declared, &payloadStart)) {
        return false;
    }
    /* 主题位于第 4 个逗号之后、第 5 个逗号之前。 */
    cursor = strstr(text, "+MQTTPUBLISH:") + strlen("+MQTTPUBLISH:");
    for (uint8_t count = 0U; count < 5U; ++count) {
        cursor = strchr(cursor, ',');
        if (cursor == (char const *)0) {
            return false;
        }
        ++cursor;
        if (count == 3U) {
            topicStart = cursor;
        } else if (count == 4U) {
            topicEnd = cursor - 1;
        }
    }
    if ((topicStart == (char const *)0) || (topicEnd == (char const *)0) ||
        (topicEnd <= topicStart)) {
        return false;
    }
    if (((size_t)(payloadStart - text) > length) ||
        ((length - (size_t)(payloadStart - text)) < (size_t)declared)) {
        return false;
    }
    topicLength = (size_t)(topicEnd - topicStart);
    if ((topicLength >= sizeof(topicText)) || ((size_t)declared >= payloadSize)) {
        return false;
    }
    (void)memcpy(topicText, topicStart, topicLength);
    topicText[topicLength] = '\0';
    (void)memcpy(payload, payloadStart, (size_t)declared);
    payload[declared] = '\0';

    for (index = 0U; index < (uint8_t)MODEM_TOPIC_COUNT; ++index) {
        if (me->topic[index].registered &&
            (strcmp(me->topic[index].name, topicText) == 0)) {
            *topic = (ModemTopic)index;
            return true;
        }
    }
    return false;   /* 未登记主题：丢弃 */
}

/*--------------------------------------------------------------- 运行期 */
/** @brief 组装一条发布命令：负载必须转成十六进制文本，命令缓冲因此约为两倍。 */
static bool mn316PublishBuild(ModemComponent *me, ModemTopic topic, char const *payload,
                              char *expected, size_t expectedSize) {
    ModemTopicSlot const *slot;
    size_t payloadLength;
    size_t used;
    int produced;

    if ((payload == (char const *)0) || (payload[0] == '\0') ||
        ((unsigned)topic >= MODEM_TOPIC_COUNT)) {
        return false;
    }
    slot = &me->topic[topic];
    if (!slot->registered) {
        return false;
    }
    payloadLength = strlen(payload);
    produced = snprintf(me->command, sizeof(me->command),
                        "AT+MQTTPUB=\"%s\",%u,0,0,%u,\"",
                        slot->name, (unsigned)slot->publish_qos,
                        (unsigned)payloadLength);
    if (!commandReady(me, produced)) {
        return false;
    }
    used = (size_t)produced;
    if ((used + (payloadLength * 2U) + 2U) > sizeof(me->command)) {
        return false;   /* 放不下就整条不发，绝不截断 */
    }
    if (!ModemAt_HexEncode(&me->command[used], sizeof(me->command) - used, payload)) {
        return false;
    }
    used += strlen(&me->command[used]);
    me->command[used] = '"';
    me->command[used + 1U] = '\0';
    /* QoS 1 要等 PUBACK；成功只代表模组收下了，不代表云端已处理。 */
    return setExpected(expected, expectedSize,
                       (slot->publish_qos == 0U) ? "OK" : "+MQTTPUBACK");
}

/** @brief 组装查小区信息的命令（复用 CEREG 的位置字段）。 */
static bool mn316CellBuild(ModemComponent *me, char *expected, size_t expectedSize) {
    if (!commandReady(me, snprintf(me->command, sizeof(me->command), "AT+CEREG?"))) {
        return false;
    }
    return setExpected(expected, expectedSize, "OK");
}

/** @brief 解析 AT+CEREG? 应答里的位置字段：+CEREG:<n>,<stat>,<tac>,<ci>,<AcT>。
* NOTE (形态，2026-09-19 实机修正)：本模组回的是**不带引号**的裸十六进制，
*   实测 `AT+CEREG=2` 之后 `AT+CEREG?` → `+CEREG:2,1,6E55,02516E85,9`。
*   原实现用 ModemAt_NextQuoted()（要求带双引号）去读，**永远读不到**，小区信息
*   会一直缺键——只有真机联调才能暴露这类问题。
*   改用 ModemAt_NextField()：它按光标处首个非空白字符是不是引号来决定读法，
*   带引号（如 ML307C 的 AT+CGI）与不带引号（本模组）两种都能读。 */
static bool mn316CellApply(ModemComponent *me) {
    char const *cursor = afterMarker(me->response, "+CEREG:");
    int32_t skip = 0;

    if (cursor == (char const *)0) {
        return false;
    }
    /* 跳过前两个字段后就是位置信息；模组未上报时按「取不到」处理。 */
    if ((!ModemAt_NextInt(&cursor, &skip)) || (!ModemAt_NextInt(&cursor, &skip))) {
        return false;
    }
    return (ModemAt_NextField(&cursor, me->data.tac, sizeof(me->data.tac))) &&
           (ModemAt_NextField(&cursor, me->data.cell_id, sizeof(me->data.cell_id)));
}

/** @brief 注册/附着长时间不上网时的补救：射频复位 + 强制自动选网。
*
* 分两阶段（用 `me->recover_phase` 记阶段，核心每次 Start 清零）：
*   0 → 1：发 `AT+CFUN=0` 关射频（把卡住的搜网状态机清掉），`MN316_RECOVER_OFF_MS` 后
*          核心会再次调用本函数；
*   1 → 2：发 `AT+CFUN=1` + `AT+COPS=0`（开射频 + 强制自动选网），再给一个完整的
*          `MODEM_COMPONENT_REG_TIMEOUT_MS` 预算继续等；
*   2     ：返回 false，不再无限补救，交给上层重连（AO 在次数用尽后会断电重启模组）。
*
* NOTE (为什么需要它，2026-09-20)：实机实测掉网后本模组停在 `+CEREG:2,0,,,`
*   （**未注册、而且不搜网**）长达十分钟，原先它没有 recover 钩子，固件毫无手段，
*   只能等 AO 用完 3 次尝试后**永久沉默**。
* NOTE (为什么不一条命令做完)：`AT+CFUN=0` 之后模组需要一点时间真正关掉射频，紧接着
*   发 `AT+CFUN=1` 常被忽略，所以拆成两次 recover 调用、中间隔 OFF_MS。
* NOTE (非阻塞)：只写串口 + 改期限，与 `ml307cRecover()` 同一套契约，不做任何等待。
*/
static bool mn316Recover(ModemComponent *me, uint8_t step) {
    static char const radioOff[] = "AT+CFUN=0\r\n";
    static char const radioOn[]  = "AT+CFUN=1\r\nAT+COPS=0\r\n";
    char const *command;
    size_t length;
    uint32_t now;
    ModemOp op = kMn316Plan[step].op;

    /* 只有「等网络」这两步值得补救；其余步骤失败说明是配置或协议问题，重试无用。 */
    if ((op != MODEM_OP_REGISTRATION) && (op != MODEM_OP_ATTACHED)) {
        return false;
    }
    now = me->port->get_tick_ms(me->port->context);
    if (me->recover_phase == 0U) {
        command = radioOff;
        length = sizeof(radioOff) - 1U;
        me->recover_phase = 1U;
        me->step_deadline_ms = now + MN316_RECOVER_OFF_MS;
    }
    else if (me->recover_phase == 1U) {
        command = radioOn;
        length = sizeof(radioOn) - 1U;
        me->recover_phase = 2U;
        me->step_deadline_ms = now + MODEM_COMPONENT_REG_TIMEOUT_MS;
    }
    else {
        return false;   /* 两阶段都用过：不再补救，让上层去断电重启 */
    }
    if (me->port->write(me->port->context, (uint8_t const *)command, length)
        != MODEM_PORT_OK) {
        return false;
    }
    me->attempt = 0U;
    me->next_attempt_ms = now + MN316_RECOVER_SETTLE_MS;
    return true;
}

/** MN316 后端实现表。 */
ModemBackend const Modem_Mn316_Backend = {
    "MN316",
    kMn316Plan,
    (uint8_t)(sizeof(kMn316Plan) / sizeof(kMn316Plan[0])),
    mn316Build,
    mn316Apply,
    mn316Recover,
    mn316UrcStarts,
    mn316UrcComplete,
    mn316UrcExtract,
    mn316PublishBuild,
    mn316CellBuild,
    mn316CellApply,
    (bool (*)(ModemComponent *, char *, size_t))0,   /* 未实现基站定位 */
    (bool (*)(ModemComponent *))0
};

#endif /* MODEM_COMPONENT_BACKEND == MODEM_BACKEND_MN316 */
