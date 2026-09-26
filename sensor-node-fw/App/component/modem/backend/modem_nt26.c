/*============================================================================
* modem_nt26.c —— NT26 后端：度云 Cat.1 bis DTU 模组的 AT 指令集
* 层次定位：Component 层 backend；只被 modem_component.c 通过 ModemBackend 调用。
*
* 模型特点：通道 + 主题登记 + 路由发送——
*   先用 AT+DTUTASK 把某路网络通道定义成 MQTT，再用 AT+MQTT / AT+MQTTAUTH 配服务器，
*   用 AT+MQTTSUB / AT+MQTTPUB 把主题登记成序号，最后用 AT+RTUWRITE="1[n]",<数据> 发送。
*   负载是纯文本，不需要十六进制编码。
*
* 来源：由 E:\stm32\stm32\Drivers\Modem\Src\backend\nt26_backend.c（阻塞版）改写：
*   - 原来的「发命令 → 就地轮询等应答」拆成一条线性步骤表，每步只负责组装命令；
*   - 等待、重试与超时都由核心按期限推进，后端里没有任何循环等待；
*   - 后端不再持有静态状态，主题序号与补救标志都放在 ModemComponent 里。
*
* 未选中本后端时整个文件不参与编译（见文件末尾的 #if）。
*===========================================================================*/
#include "modem_backend.h"
#include <stdio.h>
#include <string.h>

#if (MODEM_COMPONENT_BACKEND == MODEM_BACKEND_NT26)

/** 本后端固定使用网络通道 1；如需多服务器并行通信可把它改成配置项。 */
#define NT26_CHANNEL       (1U)

/*--------------------------------------------------------------- 计划表 */
/*! 连接计划：按顺序执行；TOPIC 步由核心按 topic_cursor 逐条展开。
*  NOTE (配置)：所有时间与次数都来自 app_config.h 的 APP_CFG_MODEM_*，
*  本表里不写裸数字，调整节奏只改配置。 */
static ModemStep const kNt26Plan[] = {
    /* op                     mode                  timeout                          retry                            budget                            attempts */
    {MODEM_OP_PROBE,          MODEM_STEP_REPEAT,    MODEM_COMPONENT_PROBE_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,     MODEM_COMPONENT_PROBE_BUDGET_MS,  MODEM_COMPONENT_PROBE_ATTEMPTS},
    {MODEM_OP_ECHO_OFF,       MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,       MODEM_COMPONENT_AT_RETRY_MS,     0U,                               1U},
    {MODEM_OP_SIM,            MODEM_STEP_REPEAT,    MODEM_COMPONENT_TIMEOUT_MS,       MODEM_COMPONENT_POLL_INTERVAL_MS, MODEM_COMPONENT_SIM_WAIT_MS,     1U},
    {MODEM_OP_IDENTITY,       MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,       MODEM_COMPONENT_AT_RETRY_MS,     0U,                               1U},
    {MODEM_OP_SIGNAL,         MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,       MODEM_COMPONENT_AT_RETRY_MS,     0U,                               1U},
    {MODEM_OP_REGISTRATION,   MODEM_STEP_REPEAT,    MODEM_COMPONENT_TIMEOUT_MS,       MODEM_COMPONENT_REG_POLL_MS,     MODEM_COMPONENT_REG_TIMEOUT_MS,   1U},
    {MODEM_OP_ATTACHED,       MODEM_STEP_REPEAT,    MODEM_COMPONENT_TIMEOUT_MS,       MODEM_COMPONENT_POLL_INTERVAL_MS, MODEM_COMPONENT_ATTACH_TIMEOUT_MS, 1U},
    {MODEM_OP_CHANNEL_CLOSE,  MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,    0U,                               1U},
    {MODEM_OP_TASK,           MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,    0U,                               1U},
    {MODEM_OP_PLATFORM,       MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,    0U,                               1U},
    {MODEM_OP_SERVER,         MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,    0U,                               1U},
    {MODEM_OP_AUTH,           MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,    0U,                               1U},
    {MODEM_OP_MSGHEAD,        MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,    0U,                               1U},
    {MODEM_OP_LBS_MODE,       MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,    0U,                               1U},
    {MODEM_OP_TOPIC_SUB,      MODEM_STEP_TOPIC,     MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,    0U,                               1U},
    {MODEM_OP_TOPIC_PUB,      MODEM_STEP_TOPIC,     MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,    0U,                               1U},
    {MODEM_OP_CHANNEL_OPEN,   MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,    0U,                               1U},
    {MODEM_OP_CHANNEL_STATE,  MODEM_STEP_REPEAT,    MODEM_COMPONENT_TIMEOUT_MS,       MODEM_COMPONENT_POLL_INTERVAL_MS, MODEM_COMPONENT_CHANNEL_TIMEOUT_MS, 1U}
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

/** @brief 定长拷贝文本字段，超长截断并始终以 '\0' 结尾。 */
static void copyField(char *destination, size_t capacity, char const *source) {
    size_t length;

    if ((destination == (char *)0) || (capacity == 0U) ||
        (source == (char const *)0)) {
        return;
    }
    length = strlen(source);
    if (length >= capacity) {
        length = capacity - 1U;
    }
    (void)memcpy(destination, source, length);
    destination[length] = '\0';
}

/*--------------------------------------------------------------- 命令组装 */
/** @brief 组装连接计划第 step 步的命令。 */
static bool nt26Build(ModemComponent *me, uint8_t step, char *expected,
                      size_t expectedSize) {
    int produced = -1;

    switch (kNt26Plan[step].op) {
        case MODEM_OP_PROBE:
            produced = snprintf(me->command, sizeof(me->command), "AT");
            break;
        case MODEM_OP_ECHO_OFF:
            produced = snprintf(me->command, sizeof(me->command), "ATE0");
            break;
        case MODEM_OP_SIM:
            produced = snprintf(me->command, sizeof(me->command), "AT+IMSI");
            break;
        case MODEM_OP_IDENTITY:
            produced = snprintf(me->command, sizeof(me->command), "AT+IMEI");
            break;
        case MODEM_OP_SIGNAL:
            produced = snprintf(me->command, sizeof(me->command), "AT+CSQ");
            break;
        case MODEM_OP_REGISTRATION:
            produced = snprintf(me->command, sizeof(me->command), "AT+CEREG");
            break;
        case MODEM_OP_ATTACHED:
            produced = snprintf(me->command, sizeof(me->command), "AT+ISLINK");
            break;
        case MODEM_OP_CHANNEL_CLOSE:
            produced = snprintf(me->command, sizeof(me->command), "AT+RTUCTL=%u,0",
                                (unsigned)NT26_CHANNEL);
            break;
        case MODEM_OP_TASK:
            produced = snprintf(me->command, sizeof(me->command),
                                "AT+DTUTASK=%u,1,\"MQTT\"", (unsigned)NT26_CHANNEL);
            break;
        case MODEM_OP_PLATFORM:
            /* 自建服务器走 normal 平台，auth1/auth2/auth3 即 client_id/用户名/密码。 */
            produced = snprintf(me->command, sizeof(me->command),
                                "AT+MQTTPLATFORM=%u,\"normal\"", (unsigned)NT26_CHANNEL);
            break;
        case MODEM_OP_SERVER:
            if (!ModemAt_IsSafeText(me->config->host)) {
                return false;
            }
            produced = snprintf(me->command, sizeof(me->command),
                                "AT+MQTT=%u,\"%s\",%u", (unsigned)NT26_CHANNEL,
                                me->config->host, (unsigned)me->config->port);
            break;
        case MODEM_OP_AUTH:
            /* NOTE (凭据)：这里只把用户名与密码写进 AT 命令，绝不写进任何日志。 */
            if ((!ModemAt_IsSafeText(me->config->device_id)) ||
                (!ModemAt_IsSafeText(me->config->username)) ||
                (!ModemAt_IsSafeText(me->config->password))) {
                return false;
            }
            produced = snprintf(me->command, sizeof(me->command),
                                "AT+MQTTAUTH=%u,\"%s\",\"%s\",\"%s\"",
                                (unsigned)NT26_CHANNEL, me->config->device_id,
                                me->config->username, me->config->password);
            break;
        case MODEM_OP_MSGHEAD:
            /* 打开消息头：下行报文会带上主题字段，关掉就只能拿到裸数据。 */
            produced = snprintf(me->command, sizeof(me->command), "AT+DTUMSGHEAD=1");
            break;
        case MODEM_OP_LBS_MODE:
            /* 多基站融合定位（误差 <100 m）；失败不影响连接，故为 OPTIONAL。 */
            produced = snprintf(me->command, sizeof(me->command),
                                "AT+LBSCFG=\"lbs_mode\",2");
            break;
        case MODEM_OP_TOPIC_SUB:
            while (me->topic_cursor < (uint8_t)MODEM_TOPIC_COUNT) {
                ModemTopicSlot const *slot = &me->topic[me->topic_cursor];
                if (slot->registered && (slot->subscribe_id != 0U)) {
                    produced = snprintf(me->command, sizeof(me->command),
                                        "AT+MQTTSUB=%u,%u,\"%s\",1",
                                        (unsigned)NT26_CHANNEL,
                                        (unsigned)slot->subscribe_id, slot->name);
                    break;
                }
                ++me->topic_cursor;   /* 该主题不需要订阅：跳过，不影响后续 */
            }
            if (me->topic_cursor >= (uint8_t)MODEM_TOPIC_COUNT) {
                return false;         /* 没有可下发的主题，核心会推进整步 */
            }
            break;
        case MODEM_OP_TOPIC_PUB:
            while (me->topic_cursor < (uint8_t)MODEM_TOPIC_COUNT) {
                ModemTopicSlot const *slot = &me->topic[me->topic_cursor];
                if (slot->registered && (slot->publish_id != 0U)) {
                    produced = snprintf(me->command, sizeof(me->command),
                                        "AT+MQTTPUB=%u,%u,\"%s\",%u,0",
                                        (unsigned)NT26_CHANNEL,
                                        (unsigned)slot->publish_id, slot->name,
                                        (unsigned)slot->publish_qos);
                    break;
                }
                ++me->topic_cursor;
            }
            if (me->topic_cursor >= (uint8_t)MODEM_TOPIC_COUNT) {
                return false;
            }
            break;
        case MODEM_OP_CHANNEL_OPEN:
            /* AT+RTUCTL 只能「恢复」本次开机已存在的连接，不能启动刚改过的新通道。 */
            produced = snprintf(me->command, sizeof(me->command), "AT+RTUCTL=%u,1",
                                (unsigned)NT26_CHANNEL);
            break;
        case MODEM_OP_CHANNEL_STATE:
            produced = snprintf(me->command, sizeof(me->command), "AT+DTUSTATE=%u",
                                (unsigned)NT26_CHANNEL);
            break;
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
static bool nt26Apply(ModemComponent *me, uint8_t step) {
    char const *cursor;
    int32_t value = 0;

    switch (kNt26Plan[step].op) {
        case MODEM_OP_SIGNAL:
            cursor = afterMarker(me->response, "+CSQ:");
            if ((cursor != (char const *)0) && ModemAt_NextInt(&cursor, &value)) {
                me->data.signal_quality = value;   /* 99 表示未知，仍算查询成功 */
            }
            return true;
        case MODEM_OP_REGISTRATION:
            cursor = afterMarker(me->response, "+CEREG:");
            if ((cursor == (char const *)0) || (!ModemAt_NextInt(&cursor, &value))) {
                return false;
            }
            /* 1 = 本地注册，5 = 漫游注册，其余都还要继续等。 */
            return (value == 1) || (value == 5);
        case MODEM_OP_ATTACHED:
            cursor = afterMarker(me->response, "+ISLINK:");
            if ((cursor == (char const *)0) || (!ModemAt_NextInt(&cursor, &value))) {
                return false;
            }
            return value == 1;
        case MODEM_OP_CHANNEL_STATE:
            cursor = afterMarker(me->response, "+DTUSTATE:");
            if ((cursor == (char const *)0) || (!ModemAt_NextInt(&cursor, &value))) {
                return false;
            }
            if (!ModemAt_NextInt(&cursor, &value)) {   /* 第二个字段才是通道状态 */
                return false;
            }
            return value == 1;
        default:
            return true;
    }
}

/** @brief 通道长时间不上线时的补救：重启模组并按落盘配置自行重连。 */
static bool nt26Recover(ModemComponent *me, uint8_t step) {
    static char const resetCommand[] = "AT+RESET\r\n";

    if (kNt26Plan[step].op != MODEM_OP_CHANNEL_STATE) {
        return false;
    }
    if (me->recover_done) {
        return false;   /* 只补救一次，避免重启循环 */
    }
    if (me->port->write(me->port->context, (uint8_t const *)resetCommand,
                        sizeof(resetCommand) - 1U) != MODEM_PORT_OK) {
        return false;
    }
    me->recover_done = true;
    me->attempt = 0U;
    me->step_deadline_ms = me->port->get_tick_ms(me->port->context) +
                           MODEM_COMPONENT_CHANNEL_TIMEOUT_MS;
    me->next_attempt_ms = me->port->get_tick_ms(me->port->context) +
                          MODEM_COMPONENT_REBOOT_SETTLE_MS;
    return true;
}

/*--------------------------------------------------------------- 主动通知 */
/** @brief 这一行是否像一条下行通知的开头。 */
static bool nt26UrcStarts(char const *text, size_t length) {
    (void)length;
    return strstr(text, "+RECV,") != (char const *)0;
}

/** @brief 定位 +RECV 通知里负载的第一个字节。 */
static char const *nt26PayloadStart(char const *recvLine) {
    char const *cursor = strchr(recvLine, ',');       /* 跳过通道号 */

    if (cursor == (char const *)0) {
        return (char const *)0;
    }
    cursor = strchr(cursor + 1, '"');                 /* 主题左引号 */
    if (cursor == (char const *)0) {
        return (char const *)0;
    }
    cursor = strchr(cursor + 1, '"');                 /* 主题右引号 */
    if (cursor == (char const *)0) {
        return (char const *)0;
    }
    cursor = strchr(cursor + 1, ',');                 /* 负载前的逗号 */
    if (cursor == (char const *)0) {
        return (char const *)0;
    }
    return cursor + 1;
}

/** @brief 通知是否已收全。
* @note 不能用「收到换行就算完」：平台下发的 JSON 常常是格式化的、负载内部带裸换行
*       （实测 {"cmd":"ping"} 也是多行），按行切会把负载截断。负载以 { 或 [ 开头时
*       按括号配平判断，其它形态仍按行尾判断。 */
static bool nt26UrcComplete(char const *buffer, size_t length) {
    char const *recvLine;
    char const *payload;
    char openChar;
    char closeChar;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    size_t index;

    if ((buffer == (char const *)0) || (length == 0U)) {
        return false;
    }
    recvLine = strstr(buffer, "+RECV,");
    if (recvLine == (char const *)0) {
        return false;
    }
    payload = nt26PayloadStart(recvLine);
    if (payload == (char const *)0) {
        return false;
    }
    if ((*payload != '{') && (*payload != '[')) {
        return strchr(recvLine, '\n') != (char const *)0;
    }
    openChar = *payload;
    closeChar = (openChar == '{') ? '}' : ']';
    for (index = (size_t)(payload - buffer); index < length; ++index) {
        char character = buffer[index];
        if (escaped) {
            escaped = false;
            continue;
        }
        if (character == '\\') {
            escaped = true;
            continue;
        }
        if (character == '"') {
            inString = !inString;
            continue;
        }
        if (inString) {
            continue;
        }
        if (character == openChar) {
            ++depth;
        } else if (character == closeChar) {
            --depth;
            if (depth == 0) {
                return true;
            }
        }
    }
    return false;
}

/** @brief 从 +RECV 通知里取出逻辑主题与负载。 */
static bool nt26UrcExtract(ModemComponent *me, char const *text, size_t length,
                           ModemTopic *topic, char *payload, size_t payloadSize) {
    static char const marker[] = "+RECV,";
    char const *cursor;
    char const *topicStart;
    char const *topicEnd;
    char const *payloadStart;
    size_t topicLength;
    size_t payloadLength;
    char topicText[MODEM_COMPONENT_TOPIC_MAX];
    uint8_t index;

    cursor = strstr(text, marker);
    if (cursor == (char const *)0) {
        return false;
    }
    cursor += sizeof(marker) - 1U;
    payloadStart = strchr(cursor, ',');
    if (payloadStart == (char const *)0) {
        return false;
    }
    topicStart = payloadStart + 1;
    if (*topicStart != '"') {
        return false;   /* SOCKET 通道的裸数据没有主题字段，无法映射 */
    }
    ++topicStart;
    topicEnd = strchr(topicStart, '"');
    if (topicEnd == (char const *)0) {
        return false;
    }
    topicLength = (size_t)(topicEnd - topicStart);
    payloadStart = topicEnd + 1;
    if (*payloadStart == ',') {
        ++payloadStart;
    }
    payloadLength = length - (size_t)(payloadStart - text);
    while ((payloadLength > 0U) &&
           ((payloadStart[payloadLength - 1U] == '\r') ||
            (payloadStart[payloadLength - 1U] == '\n'))) {
        --payloadLength;
    }
    if ((topicLength >= sizeof(topicText)) || (payloadLength >= payloadSize)) {
        return false;
    }
    (void)memcpy(topicText, topicStart, topicLength);
    topicText[topicLength] = '\0';
    (void)memcpy(payload, payloadStart, payloadLength);
    payload[payloadLength] = '\0';

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
/** @brief 组装一条发布命令；路由串形如 "1[3]"（通道 1，主题序号 3）。 */
static bool nt26PublishBuild(ModemComponent *me, ModemTopic topic, char const *payload,
                             char *expected, size_t expectedSize) {
    ModemTopicSlot const *slot;

    if ((payload == (char const *)0) || (payload[0] == '\0') ||
        ((unsigned)topic >= MODEM_TOPIC_COUNT)) {
        return false;
    }
    slot = &me->topic[topic];
    if ((!slot->registered) || (slot->publish_id == 0U)) {
        return false;
    }
    if (!commandReady(me, snprintf(me->command, sizeof(me->command),
                                   "AT+RTUWRITE=\"%u[%u]\",%s",
                                   (unsigned)NT26_CHANNEL,
                                   (unsigned)slot->publish_id, payload))) {
        return false;
    }
    return setExpected(expected, expectedSize, "OK");
}

/** @brief 组装查小区信息的命令。 */
static bool nt26CellBuild(ModemComponent *me, char *expected, size_t expectedSize) {
    if (!commandReady(me, snprintf(me->command, sizeof(me->command), "AT+CGI"))) {
        return false;
    }
    return setExpected(expected, expectedSize, "OK");
}

/** @brief 解析 AT+CGI 应答：+CGI: "<小区标识>","<跟踪区码>"。 */
static bool nt26CellApply(ModemComponent *me) {
    char const *cursor = afterMarker(me->response, "+CGI:");
    char first[MODEM_COMPONENT_CELL_FIELD_MAX];
    char second[MODEM_COMPONENT_CELL_FIELD_MAX];

    if (cursor == (char const *)0) {
        return false;
    }
    if ((!ModemAt_NextQuoted(&cursor, first, sizeof(first))) ||
        (!ModemAt_NextQuoted(&cursor, second, sizeof(second)))) {
        return false;
    }
    /* 两个字段本来就是十六进制文本，原样透传，不做数值换算。 */
    copyField(me->data.cell_id, sizeof(me->data.cell_id), first);
    copyField(me->data.tac, sizeof(me->data.tac), second);
    return true;
}

/** NT26 后端实现表。 */
ModemBackend const Modem_Nt26_Backend = {
    "NT26",
    kNt26Plan,
    (uint8_t)(sizeof(kNt26Plan) / sizeof(kNt26Plan[0])),
    nt26Build,
    nt26Apply,
    nt26Recover,
    nt26UrcStarts,
    nt26UrcComplete,
    nt26UrcExtract,
    nt26PublishBuild,
    nt26CellBuild,
    nt26CellApply,
    /* NOTE (定位未实现)：计划表里的 MODEM_OP_LBS_MODE（AT+LBSCFG="lbs_mode",2）只是把
     * 模组切成多基站融合定位模式，本后端**没有**读回坐标的步骤，两个钩子留空。
     * 因此 NT26 固件不会上报 location_lon / location_lat，核心会静默丢掉定位请求。
     * 要补齐得先确认该固件用哪条指令取经纬度（可能是 +LBS 主动上报或某条查询），
     * 再照 ml307cLocationBuild/Apply 的样子实现，切勿照抄 ML307C 的 AT+LBS。 */
    (bool (*)(ModemComponent *, char *, size_t))0,
    (bool (*)(ModemComponent *))0
};

#endif /* MODEM_COMPONENT_BACKEND == MODEM_BACKEND_NT26 */
