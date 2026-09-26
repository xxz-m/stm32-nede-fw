/*============================================================================
* modem_ml307c.c —— ML307C-DTU 后端：度云 Cat.1 DTU 模组的 AT 指令集
* 层次定位：Component 层 backend；只被 modem_component.c 通过 ModemBackend 调用。
*           未选中本后端时整个文件不参与编译（见文件末尾的 #if）。
*
* 模型特点（与 NT26 同属「度云 DTU」方言）：通道 + 主题登记 + 路由发送——
*   先用 AT+DTUTASK 把某路网络通道定义成 MQTT，再用 AT+MQTT / AT+MQTTAUTH 配服务器，
*   用 AT+MQTTSUB / AT+MQTTPUB 把主题登记成序号，最后用 AT+SENDR="1[n]",<数据> 发送。
*   打开 AT+DTUMSGHEAD 后下行是 +RECV,<通道>,"<主题>",<数据>，负载为纯文本。
*
* 与 modem_nt26.c 的差异（均于 2026-09-17 在 ML307C-GC-RTU-2.1.40 上实测确认，
*   原始串口记录见 E:\stm32\sensor-node-fw\ml307c-dtu-test-report\assets\logs）：
*   1) 没有 AT+RTUCTL：通道配置改完只能 AT+RESET，复位后模组按落盘配置自行建链
*      （实测复位到 +MQTT-CONNECT 约 11.6 s）。故本计划表用 CHANNEL_OPEN=AT+RESET
*      代替 NT26 的「打开通道」，并去掉 CHANNEL_CLOSE 步：没有指令可关通道，而
*      实测在通道已使能的状态下写 MQTTSUB/MQTTPUB 是可以的。
*   2) 上行发布用 AT+SENDR；AT+SEND 语义相同但没有任何应答，不能用于需要确认的路径。
*   3) 没有 AT+LBSCFG、也没有 AT+RTUWRITE：去掉 NT26 的 LBS 模式步，新增显式下行路由步。
*      定位改走本模组自带的 AT+LBS——它自己算基站定位并直接回经纬度，见 location_* 钩子。
*   4) AT+CGI 的字段顺序与 NT26 相同（第 1 个是小区标识、第 2 个是 TAC）。手册把这两个
*      字段的名字写反了，判据见 ml307cCellApply() 里的 NOTE（2026-09-19 在线数据定案）。
*   5) 未配置 AT+DTUPSUP：本组件的上行一律走 AT+SENDR 自带的路由串，不依赖串口
*      透传上行；实测该参数为出厂值或 "1[1]" 时连接与收发都正常。
*
* NOTE (期望应答标记为何不统一用 "OK")：ModemAt_LineHasToken() 是子串匹配，而本模组
*   重启后会主动上报 +NET: "OK"。若查询类步骤用 "OK" 作标记，这一行会被误判成应答；
*   CHANNEL_STATE 步尤其致命——它一命中核心就进入 READY，而此时通道其实还没连上。
*   因此查询类步骤一律用精确标记（+IMSI: / +CSQ: / +CEREG: / +ISLINK: / +DTUSTATE: /
*   +CGI: / +MQTTSUB: / +MQTTPUB: / +SENDR:0）。MN316 后端同样使用精确标记。
*
* NOTE (已知隐患，未在本文件处理)：ModemAt_LineHasError() 用子串匹配 "ERROR"，而本模组
*   会用 +NET: "ERROR" 主动上报掉网；该行若恰在某条命令在途时到达，那条命令会被误判为
*   ERROR。影响有限（真掉网本来就该失败重连），根治要把判定改成行首匹配，属于跨后端的
*   公共改动，另行评估，不在本次后端新增范围内。
*
* 小工具与 URC 解析沿用 modem_nt26.c 的实现（两个后端的 +RECV 格式实测一致）。每个固件
*   只编入一个后端，重复定义不会同时进入镜像；这也让 backend 保持「单文件可迁移」。
*===========================================================================*/
#include "modem_backend.h"
#include <stdio.h>
#include <string.h>

#if (MODEM_COMPONENT_BACKEND == MODEM_BACKEND_ML307C)

/** 本后端固定使用网络通道 1；如需多服务器并行通信可把它改成配置项。 */
#define ML307C_CHANNEL       (1U)

/** 下行透传路由：把本网络通道收到的数据送到串口 1 和串口 2。
*  与出厂默认值一致（实测），显式写下是为了不依赖模组里可能被改过的存量配置。 */
#define ML307C_ROUTE_DOWN    "6[1:2]"

/* 经纬度的合法上限（单位 1e-6 度）：经度 ±180、纬度 ±90。
*  AT+LBS 正常只会给出范围内的值；越界视为解析到了别的东西，当次定位作废。 */
#define ML307C_LON_E6_MAX    (180000000L)
#define ML307C_LAT_E6_MAX    (90000000L)

/*--------------------------------------------------------------- 计划表 */
/*! 连接计划：按顺序执行；TOPIC 步由核心按 topic_cursor 逐条展开。
*  NOTE (配置)：所有时间与次数都来自 app_config.h 的 APP_CFG_MODEM_*，本表不写裸数字。
*  NOTE (顺序)：AT+RESET 必须是最后一步配置动作——模组复位后才按新配置建链；它之后的
*   CHANNEL_STATE 用轮询等方式把「复位到建链完成」这段空窗兜住（实测约 11.6 s，
*   预算 CHANNEL_TIMEOUT_MS = 60 s 足够）。 */
static ModemStep const kMl307cPlan[] = {
    /* op                     mode                  timeout                            retry                             budget                              attempts */
    {MODEM_OP_PROBE,          MODEM_STEP_REPEAT,    MODEM_COMPONENT_PROBE_TIMEOUT_MS,  MODEM_COMPONENT_AT_RETRY_MS,      MODEM_COMPONENT_PROBE_BUDGET_MS,    MODEM_COMPONENT_PROBE_ATTEMPTS},
    {MODEM_OP_ECHO_OFF,       MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,        MODEM_COMPONENT_AT_RETRY_MS,      0U,                                 1U},
    {MODEM_OP_SIM,            MODEM_STEP_REPEAT,    MODEM_COMPONENT_TIMEOUT_MS,        MODEM_COMPONENT_POLL_INTERVAL_MS, MODEM_COMPONENT_SIM_WAIT_MS,        1U},
    {MODEM_OP_IDENTITY,       MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,        MODEM_COMPONENT_AT_RETRY_MS,      0U,                                 1U},
    {MODEM_OP_SIGNAL,         MODEM_STEP_OPTIONAL,  MODEM_COMPONENT_TIMEOUT_MS,        MODEM_COMPONENT_AT_RETRY_MS,      0U,                                 1U},
    {MODEM_OP_REGISTRATION,   MODEM_STEP_REPEAT,    MODEM_COMPONENT_TIMEOUT_MS,        MODEM_COMPONENT_REG_POLL_MS,      MODEM_COMPONENT_REG_TIMEOUT_MS,     1U},
    {MODEM_OP_ATTACHED,       MODEM_STEP_REPEAT,    MODEM_COMPONENT_TIMEOUT_MS,        MODEM_COMPONENT_POLL_INTERVAL_MS, MODEM_COMPONENT_ATTACH_TIMEOUT_MS,  1U},
    {MODEM_OP_TASK,           MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,      0U,                                 1U},
    /* NOTE (顺序，2026-09-19 修正)：SERVER 必须先于 PLATFORM。2026-09-17 实机跑通的序列
     * 与厂默认值的存储顺序都是 AT+MQTT(服务器) → AT+MQTTPLATFORM，原表把两步写反了。 */
    {MODEM_OP_SERVER,         MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,      0U,                                 1U},
    {MODEM_OP_PLATFORM,       MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,      0U,                                 1U},
    {MODEM_OP_AUTH,           MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,      0U,                                 1U},
    {MODEM_OP_MSGHEAD,        MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,      0U,                                 1U},
    {MODEM_OP_ROUTE_DOWN,     MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,      0U,                                 1U},
    {MODEM_OP_TOPIC_SUB,      MODEM_STEP_TOPIC,     MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,      0U,                                 1U},
    {MODEM_OP_TOPIC_PUB,      MODEM_STEP_TOPIC,     MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,      0U,                                 1U},
    {MODEM_OP_CHANNEL_OPEN,   MODEM_STEP_ONCE,      MODEM_COMPONENT_CONFIG_TIMEOUT_MS, MODEM_COMPONENT_AT_RETRY_MS,      0U,                                 1U},
    {MODEM_OP_CHANNEL_STATE,  MODEM_STEP_REPEAT,    MODEM_COMPONENT_TIMEOUT_MS,        MODEM_COMPONENT_POLL_INTERVAL_MS, MODEM_COMPONENT_CHANNEL_TIMEOUT_MS,  1U}
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
static bool ml307cBuild(ModemComponent *me, uint8_t step, char *expected,
                        size_t expectedSize) {
    int produced = -1;
    char const *token = "OK";

    switch (kMl307cPlan[step].op) {
        case MODEM_OP_PROBE:
            produced = snprintf(me->command, sizeof(me->command), "AT");
            break;
        case MODEM_OP_ECHO_OFF:
            /* 手册索引里没有 ATE 条目，不支持时会回 ERROR；该步是 OPTIONAL，
             * 失败也继续，不影响连接。本机实测模组默认不回显，这一条是保险。 */
            produced = snprintf(me->command, sizeof(me->command), "ATE0");
            break;
        case MODEM_OP_SIM:
            produced = snprintf(me->command, sizeof(me->command), "AT+IMSI");
            token = "+IMSI:";
            break;
        case MODEM_OP_IDENTITY:
            produced = snprintf(me->command, sizeof(me->command), "AT+IMEI");
            token = "+IMEI:";
            break;
        case MODEM_OP_SIGNAL:
            produced = snprintf(me->command, sizeof(me->command), "AT+CSQ");
            token = "+CSQ:";
            break;
        case MODEM_OP_REGISTRATION:
            produced = snprintf(me->command, sizeof(me->command), "AT+CEREG");
            token = "+CEREG:";
            break;
        case MODEM_OP_ATTACHED:
            /* ML307C 用它表示「数据链路已建立」，与 DTU 通道是否上线无关，
             * 因此这里读到的 1 只说明可以开始配 MQTT。 */
            produced = snprintf(me->command, sizeof(me->command), "AT+ISLINK");
            token = "+ISLINK:";
            break;
        case MODEM_OP_TASK:
            produced = snprintf(me->command, sizeof(me->command),
                                "AT+DTUTASK=%u,1,\"MQTT\"", (unsigned)ML307C_CHANNEL);
            break;
        case MODEM_OP_PLATFORM:
            /* 自建服务器走 normal 平台，auth1/auth2/auth3 即 client_id/用户名/密码。 */
            produced = snprintf(me->command, sizeof(me->command),
                                "AT+MQTTPLATFORM=%u,\"normal\"", (unsigned)ML307C_CHANNEL);
            break;
        case MODEM_OP_SERVER:
            if (!ModemAt_IsSafeText(me->config->host)) {
                return false;
            }
            produced = snprintf(me->command, sizeof(me->command),
                                "AT+MQTT=%u,\"%s\",%u", (unsigned)ML307C_CHANNEL,
                                me->config->host, (unsigned)me->config->port);
            break;
        case MODEM_OP_AUTH:
            /* NOTE (凭据)：这里只把用户名与密码写进 AT 命令，绝不写进任何日志。
             * NOTE (参数个数，2026-09-19 修正)：本模组要 4 个字符串参数 auth1..auth4，
             *   实测只发 3 个会回 +CME ERROR: 4（参数形态错误，2026-09-19 上板踩到）。
             *   auth1=client_id、auth2=用户名、auth3=密码、auth4=用户标识；normal 平台
             *   把 auth4 留空，与厂默认值 AT+MQTTAUTH=1,"<#IMEI>A","","","" 一致。 */
            if ((!ModemAt_IsSafeText(me->config->device_id)) ||
                (!ModemAt_IsSafeText(me->config->username)) ||
                (!ModemAt_IsSafeText(me->config->password))) {
                return false;
            }
            produced = snprintf(me->command, sizeof(me->command),
                                "AT+MQTTAUTH=%u,\"%s\",\"%s\",\"%s\",\"\"",
                                (unsigned)ML307C_CHANNEL, me->config->device_id,
                                me->config->username, me->config->password);
            break;
        case MODEM_OP_MSGHEAD:
            /* 打开消息头：下行报文会带上主题字段，关掉就只能拿到裸数据，
             * 而本组件的下行分发要靠主题名做映射。 */
            produced = snprintf(me->command, sizeof(me->command), "AT+DTUMSGHEAD=1");
            break;
        case MODEM_OP_ROUTE_DOWN:
            produced = snprintf(me->command, sizeof(me->command),
                                "AT+DTUPSDN=%u,\"%s\"", (unsigned)ML307C_CHANNEL,
                                ML307C_ROUTE_DOWN);
            break;
        case MODEM_OP_TOPIC_SUB:
            while (me->topic_cursor < (uint8_t)MODEM_TOPIC_COUNT) {
                ModemTopicSlot const *slot = &me->topic[me->topic_cursor];
                if (slot->registered && (slot->subscribe_id != 0U)) {
                    produced = snprintf(me->command, sizeof(me->command),
                                        "AT+MQTTSUB=%u,%u,\"%s\",%u",
                                        (unsigned)ML307C_CHANNEL,
                                        (unsigned)slot->subscribe_id, slot->name,
                                        (unsigned)slot->publish_qos);
                    token = "+MQTTSUB:";
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
                    /* NOTE (参数个数)：ML307C 手册与实测都是 4 个参数
                     *   <id>,<topic_id>,"<topic>",<qos>；NT26 那份多一个 retain。 */
                    produced = snprintf(me->command, sizeof(me->command),
                                        "AT+MQTTPUB=%u,%u,\"%s\",%u",
                                        (unsigned)ML307C_CHANNEL,
                                        (unsigned)slot->publish_id, slot->name,
                                        (unsigned)slot->publish_qos);
                    token = "+MQTTPUB:";
                    break;
                }
                ++me->topic_cursor;
            }
            if (me->topic_cursor >= (uint8_t)MODEM_TOPIC_COUNT) {
                return false;
            }
            break;
        case MODEM_OP_CHANNEL_OPEN:
            /* 本模组没有 AT+RTUCTL，通道参数只能在复位后生效：这里发 AT+RESET，
             * 模组会回 +RESET / OK，然后重启并按刚写入的配置自行建链。
             * NOTE (标记用 "OK" 而不是 "+RESET")：OK 是必然出现的终止行，兼容性更好；
             *   此刻模组还没重启，被 +NET: "OK" 抢答的概率极低，而且真被抢答也无害——
             *   下一步 CHANNEL_STATE 会一直轮询到通道真的上线才放行。 */
            produced = snprintf(me->command, sizeof(me->command), "AT+RESET");
            break;
        case MODEM_OP_CHANNEL_STATE:
            produced = snprintf(me->command, sizeof(me->command), "AT+DTUSTATE=%u",
                                (unsigned)ML307C_CHANNEL);
            token = "+DTUSTATE:";
            break;
        default:
            return false;
    }
    if (!commandReady(me, produced)) {
        return false;
    }
    return setExpected(expected, expectedSize, token);
}

/*--------------------------------------------------------------- 应答解析 */
/** @brief 解析第 step 步的应答；返回 false 表示这一步其实没通过。 */
static bool ml307cApply(ModemComponent *me, uint8_t step) {
    char const *cursor;
    int32_t value = 0;

    switch (kMl307cPlan[step].op) {
        case MODEM_OP_SIM: {
            /* 卡未就绪时模组可能回空 IMSI：只看到 +IMSI: 不算通过，要继续等。 */
            char imsi[24];
            cursor = afterMarker(me->response, "+IMSI:");
            return (cursor != (char const *)0) &&
                   ModemAt_NextQuoted(&cursor, imsi, sizeof(imsi));
        }
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
            /* 应答形如 +DTUSTATE: 1,1；第二个字段才是通道状态。 */
            cursor = afterMarker(me->response, "+DTUSTATE:");
            if ((cursor == (char const *)0) || (!ModemAt_NextInt(&cursor, &value))) {
                return false;
            }
            if (!ModemAt_NextInt(&cursor, &value)) {
                return false;
            }
            return value == 1;
        default:
            return true;
    }
}

/** @brief 通道长时间不上线时的补救：重启模组并按落盘配置自行重连。
* @note 本后端的 CHANNEL_OPEN 本身就是一次复位，所以这里再复位一次收益有限，但它是
*   「MQTT 栈卡死」这类软故障唯一可用的手段；核心保证只补救一次（recover_done），
*   最坏情况是失败判定被拉长到约 2 分钟，之后仍由 AO 的 CONNECT_ATTEMPTS 收尾。 */
static bool ml307cRecover(ModemComponent *me, uint8_t step) {
    static char const resetCommand[] = "AT+RESET\r\n";

    if (kMl307cPlan[step].op != MODEM_OP_CHANNEL_STATE) {
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
static bool ml307cUrcStarts(char const *text, size_t length) {
    (void)length;
    return strstr(text, "+RECV,") != (char const *)0;
}

/** @brief 定位 +RECV 通知里负载的第一个字节。 */
static char const *ml307cPayloadStart(char const *recvLine) {
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
* @note 不能用「收到换行就算完」：负载内部可能带裸换行，按行切会把负载截断。
*       负载以 { 或 [ 开头时按括号配平判断，其它形态仍按行尾判断。 */
static bool ml307cUrcComplete(char const *buffer, size_t length) {
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
    payload = ml307cPayloadStart(recvLine);
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
static bool ml307cUrcExtract(ModemComponent *me, char const *text, size_t length,
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
/** @brief 组装一条发布命令；路由串形如 "1[3]"（通道 1，发布主题序号 3）。 */
static bool ml307cPublishBuild(ModemComponent *me, ModemTopic topic, char const *payload,
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
                                   "AT+SENDR=\"%u[%u]\",%s",
                                   (unsigned)ML307C_CHANNEL,
                                   (unsigned)slot->publish_id, payload))) {
        return false;
    }
    /* NOTE (成功判据)：AT+SENDR 会回 +SENDR:0（成功推入发送区）或 +SENDR:-1（推送失败），
     * 之后才是 OK。因此用 "+SENDR:0" 而不是 "OK" 作标记——用 "OK" 会把 -1 也算成功，
     * 变成「看起来发出去了其实没有」的静默假成功。手册也说明 0 只代表推入发送区，
     * 不代表服务器收到；真正的送达确认要靠下行应答或云端状态。 */
    return setExpected(expected, expectedSize, "+SENDR:0");
}

/** @brief 组装查小区信息的命令。 */
static bool ml307cCellBuild(ModemComponent *me, char *expected, size_t expectedSize) {
    if (!commandReady(me, snprintf(me->command, sizeof(me->command), "AT+CGI"))) {
        return false;
    }
    return setExpected(expected, expectedSize, "+CGI:");
}

/** @brief 解析 AT+CGI 应答：详情见函数内的字段顺序说明。 */
static bool ml307cCellApply(ModemComponent *me) {
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
    /* NOTE (字段顺序，2026-09-19 按在线数据修正)：**手册把两个字段的名字写反了**。
     * 手册写的是 +CGI: "<lac>","<cell_id>"（它自己的示例也是 +CGI: "85be00a","1f52"），
     * 照此落位会把小区标识当 TAC 上报。三条判据都指向「第 1 个是小区标识、第 2 个是 TAC」：
     *   ① 位宽：TAC/LAC 是 16 bit（最多 4 位十六进制），ECI 小区标识是 28 bit（7 位）。
     *      实测与手册示例的第 1 个字段都是 7 位（250ba81 / 85be00a），第 2 个都是 4 位
     *      （146c / 1f52）——形态与手册的名字正好相反。
     *   ② 自洽性：2026-09-17 第 1 个字段为 250ba81，2026-09-19 变为 6cf60ca，而第 2 个
     *      字段两次都是 146c。同一个小区不可能换跟踪区，故「第 2 个 = TAC」才自洽；
     *      反过来的读法会得出「小区没变、跟踪区变了」这个物理上不可能的组合。
     *   ③ 同族固件：NT26 是同一套度云 DTU 固件，nt26CellApply() 取的就是「小区标识在前、
     *      TAC 在后」，两个后端的 +RECV 格式实测也一致，没有理由只有这一项相反。
     * 因此这里按 NT26 的顺序落位。两个字段都是十六进制文本，原样透传、不做数值换算。
     * 若日后发现平台侧另有约定，改回两行即可（本 NOTE 保留了完整判据）。 */
    copyField(me->data.cell_id, sizeof(me->data.cell_id), first);
    copyField(me->data.tac, sizeof(me->data.tac), second);
    return true;
}

/** @brief 组装基站定位查询。
* @note 手册的语法是 AT+LBS? 且注明「? 可省略」，这里按不带 ? 的形式发（与 AT+CSQ /
*   AT+IMEI 的做法一致，实测那两条也是同一种写法）。 */
static bool ml307cLocationBuild(ModemComponent *me, char *expected, size_t expectedSize) {
    if (!commandReady(me, snprintf(me->command, sizeof(me->command), "AT+LBS"))) {
        return false;
    }
    return setExpected(expected, expectedSize, "+LBS:");
}

/** @brief 解析 AT+LBS 应答：+LBS: "<经度>","<纬度>"，单位「度」的十进制定点文本。
* @note 手册示例 +LBS: "104.071319","30.690876"；失败时模组回 +CME ERROR: 130，
*   那条路径由 AT 引擎判成 ERROR，根本走不到这里（核心按「尽力而为」处理）。
* NOTE (为什么用整数)：ModemAt_ParseE6() 把定点文本按 1e-6 度折成整数，绕开 Cortex-M3
*   上没有硬件浮点的浮点库；1e-6 度约 0.11 m，远小于基站定位几百米的误差，够用。
* NOTE (先算完再提交)：经纬度各解析一次，两个都成功才写进 data——否则会出现「经度已更新、
*   纬度还是上一次的」这种不配对的组合，比干脆不更新更糟。 */
static bool ml307cLocationApply(ModemComponent *me) {
    char const *cursor = afterMarker(me->response, "+LBS:");
    char longitude[24];
    char latitude[24];
    int32_t lon_e6 = 0;
    int32_t lat_e6 = 0;

    if (cursor == (char const *)0) {
        return false;
    }
    if ((!ModemAt_NextQuoted(&cursor, longitude, sizeof(longitude))) ||
        (!ModemAt_NextQuoted(&cursor, latitude, sizeof(latitude)))) {
        return false;
    }
    /* 取值范围检查：经度 ±180、纬度 ±90（单位 1e-6 度）。越界说明解析到的不是坐标，
     * 宁可当这次定位失败，也不要往上报一个离谱的值。 */
    if ((!ModemAt_ParseE6(longitude, &lon_e6)) ||
        (lon_e6 > ML307C_LON_E6_MAX) || (lon_e6 < -ML307C_LON_E6_MAX)) {
        return false;
    }
    if ((!ModemAt_ParseE6(latitude, &lat_e6)) ||
        (lat_e6 > ML307C_LAT_E6_MAX) || (lat_e6 < -ML307C_LAT_E6_MAX)) {
        return false;
    }
    me->data.lon_e6 = lon_e6;
    me->data.lat_e6 = lat_e6;
    /* (0,0) 在几内亚湾，基站定位不会给出这个点，出现即视为本次定位无效。 */
    me->data.location_valid = !((lon_e6 == 0) && (lat_e6 == 0));
    return true;
}

/** ML307C 后端实现表。 */
ModemBackend const Modem_Ml307c_Backend = {
    "ML307C",
    kMl307cPlan,
    (uint8_t)(sizeof(kMl307cPlan) / sizeof(kMl307cPlan[0])),
    ml307cBuild,
    ml307cApply,
    ml307cRecover,
    ml307cUrcStarts,
    ml307cUrcComplete,
    ml307cUrcExtract,
    ml307cPublishBuild,
    ml307cCellBuild,
    ml307cCellApply,
    ml307cLocationBuild,
    ml307cLocationApply
};

#endif /* MODEM_COMPONENT_BACKEND == MODEM_BACKEND_ML307C */
