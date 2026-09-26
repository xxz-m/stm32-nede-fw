/*============================================================================
* modem_component.c —— 4G 云业务核心：非阻塞连接状态机 + 业务协议
* 层次定位：Component 层核心；业务协议（主题组织、上报报文、下行分发）在这里，
*           模组专有指令与 URC 解析在 backend/ 里。
* 目标硬件：与具体模组无关
*
* 非阻塞约定（为什么必须这样写）：QV 是协作式内核，任何一个状态处理函数不返回，
*   全系统停摆。因此本文件里没有一处「等模组应答」的循环：
*     发起（写命令 + 记期限） → 收行（喂给 AT 引擎） → 判结果（命中/超时）
*   三步分别发生在三次 Process 调用里，中间靠时间事件与串口事件推进。
*
* 唯一真相：主题名、方向、QoS 与上报字段在这里定义；后端只负责把逻辑主题翻译成
*   自家形式（主题字符串或"通道[序号]"路由串）。
*===========================================================================*/
#include "modem_component.h"
#include "modem_backend.h"
#include <stdio.h>
#include <string.h>

/*--------------------------------------------------------------- 主题定义 */
/*! 主题后缀；与云端约定的 /{productId}/{deviceId}/{suffix} 一一对应。 */
static char const *const kTopicSuffix[MODEM_TOPIC_COUNT] = {
    "properties/report",
    "properties/read",
    "properties/read/reply",
    "properties/write",
    "properties/write/reply",
    "function/invoke",
    "state-check",
    "state-check/reply"
};

/*! 每类逻辑主题的收发方向；决定后端把它登记为订阅、发布还是两者。 */
static ModemDirection const kTopicDirection[MODEM_TOPIC_COUNT] = {
    MODEM_DIR_PUBLISH,    /* MODEM_TOPIC_REPORT      */
    MODEM_DIR_SUBSCRIBE,  /* MODEM_TOPIC_READ        */
    MODEM_DIR_PUBLISH,    /* MODEM_TOPIC_READ_REPLY  */
    MODEM_DIR_SUBSCRIBE,  /* MODEM_TOPIC_WRITE       */
    MODEM_DIR_PUBLISH,    /* MODEM_TOPIC_WRITE_REPLY */
    MODEM_DIR_SUBSCRIBE,  /* MODEM_TOPIC_INVOKE      */
    MODEM_DIR_SUBSCRIBE,  /* MODEM_TOPIC_STATE       */
    MODEM_DIR_PUBLISH     /* MODEM_TOPIC_STATE_REPLY */
};

/*--------------------------------------------------------------- 小工具 */
static uint32_t nowMs(ModemComponent const *me) {
    return me->port->get_tick_ms(me->port->context);
}

/** @brief 向字符串末尾追加文本；容量不足返回 false（不做部分写入）。 */
static bool append(char *buffer, size_t capacity, size_t *used, char const *text) {
    size_t length = strlen(text);
    if ((*used + length + 1U) > capacity) {
        return false;
    }
    (void)memcpy(&buffer[*used], text, length + 1U);
    *used += length;
    return true;
}

/** @brief 把整数格式化成文本（不用 stdlib，避免把 strtol/printf 的大实现拉进固件）。 */
static bool formatInteger(char *out, size_t capacity, int32_t value) {
    int length = snprintf(out, capacity, "%ld", (long)value);
    return (length >= 0) && ((size_t)length < capacity);
}

/** @brief 把定点整数格式化成文本，小数点位置由 decimals 指定。 */
static bool formatDecimal(char *out, size_t capacity, int32_t value, uint8_t decimals) {
    uint32_t magnitude;
    uint32_t divisor = 1U;
    uint32_t remainder;
    uint32_t step;
    char fraction[16];
    int length;
    uint8_t index;

    if (decimals == 0U) {
        return formatInteger(out, capacity, value);
    }
    if (decimals > 9U) {
        return false;
    }
    for (index = 0U; index < decimals; ++index) {
        divisor *= 10U;
    }
    /* NOTE (负数)：用无符号补码取绝对值，避免 INT32_MIN 取负溢出。 */
    magnitude = (value < 0) ? (uint32_t)(0U - (uint32_t)value) : (uint32_t)value;
    remainder = magnitude % divisor;
    step = divisor / 10U;
    for (index = 0U; index < decimals; ++index) {
        fraction[index] = (char)('0' + ((remainder / step) % 10U));
        step /= 10U;
    }
    fraction[decimals] = '\0';
    length = snprintf(out, capacity, "%s%lu.%s", (value < 0) ? "-" : "",
                      (unsigned long)(magnitude / divisor), fraction);
    return (length >= 0) && ((size_t)length < capacity);
}

/** @brief 把纯文本格式化成原样输出（调用方保证内容不含引号与反斜杠）。 */
static bool formatText(char *out, size_t capacity, char const *text) {
    size_t length = strlen(text);
    if ((length + 1U) > capacity) {
        return false;
    }
    (void)memcpy(out, text, length + 1U);
    return true;
}

/** @brief 追加 ",\"name\":<整数值>". */
static bool appendInteger(char *buffer, size_t capacity, size_t *used,
                          char const *name, int32_t value) {
    char text[32];
    char part[48];
    int length;

    if (!formatInteger(text, sizeof(text), value)) {
        return false;
    }
    length = snprintf(part, sizeof(part), ",\"%s\":%s", name, text);
    if ((length < 0) || ((size_t)length >= sizeof(part))) {
        return false;
    }
    return append(buffer, capacity, used, part);
}

/** @brief 追加 ",\"name\":<定点值>". */
static bool appendDecimal(char *buffer, size_t capacity, size_t *used,
                          char const *name, int32_t value, uint8_t decimals) {
    char text[32];
    char part[64];
    int length;

    if (!formatDecimal(text, sizeof(text), value, decimals)) {
        return false;
    }
    length = snprintf(part, sizeof(part), ",\"%s\":%s", name, text);
    if ((length < 0) || ((size_t)length >= sizeof(part))) {
        return false;
    }
    return append(buffer, capacity, used, part);
}

/** @brief 追加 ",\"name\":\"text\""（文本按原样放入，不做转义；调用方保证内容是十六进制或纯文本）。 */
static bool appendText(char *buffer, size_t capacity, size_t *used,
                       char const *name, char const *text) {
    char part[96];
    int length = snprintf(part, sizeof(part), ",\"%s\":\"%s\"", name, text);
    if ((length < 0) || ((size_t)length >= sizeof(part))) {
        return false;
    }
    return append(buffer, capacity, used, part);
}

/** @brief 从约定格式的 JSON 文本里取字符串字段（轻量提取，不是完整解析器）。 */
static bool jsonString(char const *json, char const *key, char *out, size_t capacity) {
    char pattern[32];
    char const *cursor;
    char const *end;
    size_t length;
    int produced = snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    if ((produced < 0) || ((size_t)produced >= sizeof(pattern))) {
        return false;
    }
    cursor = strstr(json, pattern);
    if (cursor == (char const *)0) {
        return false;
    }
    cursor += strlen(pattern);
    while (*cursor == ' ') {
        ++cursor;
    }
    if (*cursor != ':') {
        return false;
    }
    ++cursor;
    while (*cursor == ' ') {
        ++cursor;
    }
    if (*cursor != '"') {
        return false;
    }
    ++cursor;
    end = strchr(cursor, '"');
    if (end == (char const *)0) {
        return false;
    }
    length = (size_t)(end - cursor);
    if (length >= capacity) {
        return false;
    }
    (void)memcpy(out, cursor, length);
    out[length] = '\0';
    return true;
}

/** @brief 从约定格式的 JSON 文本里取数值 / 布尔字段的文本形式。 */
static bool jsonValue(char const *json, char const *key, char *out, size_t capacity) {
    char pattern[32];
    char const *cursor;
    char const *end;
    size_t length;
    int produced = snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    if ((produced < 0) || ((size_t)produced >= sizeof(pattern))) {
        return false;
    }
    cursor = strstr(json, pattern);
    if (cursor == (char const *)0) {
        return false;
    }
    cursor += strlen(pattern);
    while (*cursor == ' ') {
        ++cursor;
    }
    if (*cursor != ':') {
        return false;
    }
    ++cursor;
    while (*cursor == ' ') {
        ++cursor;
    }
    end = cursor;
    while ((*end != '\0') && (*end != ',') && (*end != '}') && (*end != ' ')) {
        ++end;
    }
    length = (size_t)(end - cursor);
    if ((length == 0U) || (length >= capacity)) {
        return false;
    }
    (void)memcpy(out, cursor, length);
    out[length] = '\0';
    return true;
}

/** @brief 解析十进制整数（自定义实现，避免依赖 strtol）。 */
static bool parseInt32(char const *text, int32_t *out) {
    int32_t value = 0;
    bool negative = false;

    if ((text == (char const *)0) || (*text == '\0')) {
        return false;
    }
    if (*text == '-') {
        negative = true;
        ++text;
    }
    if (*text == '\0') {
        return false;
    }
    while ((*text >= '0') && (*text <= '9')) {
        value = (value * 10) + (int32_t)(*text - '0');
        ++text;
    }
    if (*text != '\0') {
        return false;
    }
    *out = negative ? -value : value;
    return true;
}

/*--------------------------------------------------------------- 主题组装 */
/** @brief 按产品标识、设备标识与后缀拼出全部主题，并分配订阅/发布序号。 */
static void buildTopics(ModemComponent *me) {
    uint8_t index;

    me->next_subscribe_id = 1U;
    me->next_publish_id = 1U;
    for (index = 0U; index < MODEM_TOPIC_COUNT; ++index) {
        ModemTopicSlot *slot = &me->topic[index];
        int produced = snprintf(slot->name, sizeof(slot->name), "/%s/%s/%s",
                                me->config->product_id, me->config->device_id,
                                kTopicSuffix[index]);

        slot->direction = kTopicDirection[index];
        /* QoS 由用途决定：周期数据用 0，命令应答用 1（NT26 在登记主题时固定）。 */
        slot->publish_qos = (index == (uint8_t)MODEM_TOPIC_REPORT) ? 0U : 1U;
        slot->subscribe_id = 0U;
        slot->publish_id = 0U;
        if ((produced < 0) || ((size_t)produced >= sizeof(slot->name))) {
            slot->name[0] = '\0';
            slot->registered = false;
            continue;
        }
        slot->registered = true;
        if (slot->direction != MODEM_DIR_PUBLISH) {
            slot->subscribe_id = me->next_subscribe_id++;
        }
        if (slot->direction != MODEM_DIR_SUBSCRIBE) {
            slot->publish_id = me->next_publish_id++;
        }
    }
}

/*--------------------------------------------------------------- 串口收取 */
/** @brief 把一整行交给主动通知拦截器或 AT 引擎。 */
static void feedLine(ModemComponent *me, char const *line, uint32_t now) {
    ModemBackend const *backend = Modem_Backend_Get();
    size_t length = strlen(line);

    /* 通知优先：负载里常常含裸换行，若当应答文本匹配，负载里恰好出现的 OK
     * 会让状态机误判。已有一条收全的通知时不再接收新行，等上层分发完再说。 */
    if (me->urc_len > 0U) {
        if (!backend->urc_complete(me->urc, me->urc_len)) {
            if ((me->urc_len + length + 1U) <= sizeof(me->urc)) {
                (void)memcpy(&me->urc[me->urc_len], line, length);
                me->urc_len += length;
                me->urc[me->urc_len] = '\0';
                me->urc_ready = backend->urc_complete(me->urc, me->urc_len);
                return;
            }
            /* 缓冲不够：整条丢弃，避免半截报文被解析成假的下行命令。 */
            me->urc_len = 0U;
            me->urc[0] = '\0';
            me->urc_ready = false;
            MODEM_TRACE(me, "notification overflow; dropped");
            return;
        }
    } else if (backend->urc_starts(line, length)) {
        if ((length + 1U) > sizeof(me->urc)) {
            MODEM_TRACE(me, "notification line too long; dropped");
            return;
        }
        (void)memcpy(me->urc, line, length);
        me->urc_len = length;
        me->urc[me->urc_len] = '\0';
        me->urc_ready = backend->urc_complete(me->urc, me->urc_len);
        return;
    }
    if (me->at.phase != MODEM_AT_WAITING) {
        return;   /* 空闲时与通知无关的行（模组日志）直接丢 */
    }
    /* 应答文本整体留档：入网阶段要从中取 +CEREG / +CSQ / 通道状态。 */
    if ((me->response_len + length + 2U) <= sizeof(me->response)) {
        (void)memcpy(&me->response[me->response_len], line, length);
        me->response_len += length;
        me->response[me->response_len] = '\n';
        me->response_len++;
        me->response[me->response_len] = '\0';
    }
    (void)ModemAt_Feed(&me->at, line, length, now);
}

/** @brief 收字节组行；一行收满就交给 feedLine。 */
static void feedByte(ModemComponent *me, uint8_t byte, uint32_t now) {
    if ((byte == (uint8_t)'\n') || (byte == (uint8_t)'\r') || (byte == 0U)) {
        if (me->line_len > 0U) {
            me->line[me->line_len] = '\0';
            feedLine(me, me->line, now);
            me->line_len = 0U;
        }
        me->line_overflow = false;
        return;
    }
    if (me->line_overflow) {
        return;   /* 整行已判超长，等行尾再恢复 */
    }
    if (me->line_len < (sizeof(me->line) - 1U)) {
        me->line[me->line_len++] = (char)byte;
    } else {
        me->line_overflow = true;
        me->line_len = 0U;
        MODEM_TRACE(me, "line too long; dropped");
    }
}

/** @brief 读走串口当前已有的字节（最多 MODEM_COMPONENT_RX_BUDGET 个）。 */
static void readPort(ModemComponent *me, uint32_t now) {
    uint8_t buffer[MODEM_COMPONENT_RX_BUDGET];
    size_t count;
    size_t index;

    count = me->port->read(me->port->context, buffer, sizeof(buffer));
    for (index = 0U; index < count; ++index) {
        feedByte(me, buffer[index], now);
    }
}

/*--------------------------------------------------------------- 计划推进 */
/** @brief 由语义动作推导组件状态，供上层显示与日志使用。 */
static ModemComponentState stateOfOp(ModemOp op) {
    if (op == MODEM_OP_PROBE) {
        return MODEM_COMPONENT_PROBING;
    }
    /* MODEM_OP_PDP 虽然追加在枚举末尾，语义上属于「入网」阶段（查 APN），
     * 这里单独放行，只是为了让状态显示准确，与调度无关。 */
    if ((op <= MODEM_OP_IP_ADDRESS) || (op == MODEM_OP_PDP)) {
        return MODEM_COMPONENT_NETWORK;
    }
    return MODEM_COMPONENT_MQTT;
}

static void startPlanStep(ModemComponent *me, uint32_t now);
static void stepFailed(ModemComponent *me, ModemStep const *step, uint32_t now);

/** @brief 把连接计划标记为完成，进入在线态。 */
static void finishPlan(ModemComponent *me) {
    ModemAt_Reset(&me->at);
    me->runtime_op = MODEM_RUNTIME_NONE;
    me->report_period_ms = (uint32_t)me->config->report_period_s * 1000U;
    if (me->report_period_ms == 0U) {
        me->report_period_ms = (uint32_t)MODEM_COMPONENT_REPORT_PERIOD_S * 1000U;
    }
    me->cell_period_ms = (uint32_t)MODEM_COMPONENT_CELL_PERIOD_S * 1000U;
    me->report_requested = true;   /* 连上先发首报，让平台立刻看到设备 */
    me->state = MODEM_COMPONENT_READY;
    MODEM_TRACE(me, "cloud link ready");
}

/** @brief 连接失败：停在 FAILED，由上层决定何时重连。 */
static void failPlan(ModemComponent *me, ModemFailure failure) {
    ModemAt_Reset(&me->at);
    me->runtime_op = MODEM_RUNTIME_NONE;
    me->reply_pending = false;
    me->failure = failure;
    me->state = MODEM_COMPONENT_FAILED;
    MODEM_TRACE(me, "cloud link failed");
}

/*--------------------------------------------------------------- 步骤诊断 */
/*! 诊断文本里单个字段的最大长度；够放下一条典型 AT 命令，超出截断。 */
/*! 诊断行的三个字段上限。
*  NOTE (命令字段为什么故意短，2026-09-27)：`cmd="…"` 会整条打印，而
*   `AT+MQTTCFG=` 里带着 **用户名与密码**。36 字符恰好截在 host 之后就把凭据挡住了，
*   因此这个值**不要随意调大**——调大等于把凭据写进日志，违反 AGENTS.md 4.2 的凭据纪律。
*   真要看清参数尾部，正确做法是给组件加一个「可安全打印的命令」缓冲 + 后端填它的钩子
*   （手册第 8 页的读回形式本身就是 `+MQTTCFG:<host>,…,<user>,"******",…`）。
*  NOTE (应答字段为什么要大一些)：AT 应答是多行的——模组先回命令确认（`OK`），
*   随后才用 URC 上报真正的结果。只取首行会把「模组明确说了失败」显示成「模组没说话」。 */
#define MODEM_DIAG_FIELD_MAX   (36U)
#define MODEM_DIAG_REPLY_MAX   (72U)
#define MODEM_DIAG_LINE_MAX    (168U)

#if (MODEM_COMPONENT_TRACE_ENABLE != 0U)
/** @brief 把源文本压成一行可打印 ASCII：只取第一行，非可打印字符换成 '.'。
*  NOTE (为什么清洗)：AT 命令尾部带 CRLF，应答也可能是多行；原样进日志会把一行
*   拆成几行并混入控制字符，而运行期输出必须保持 ASCII 单行（见 AGENTS.md 5.2）。 */
static void describeField(char const *source, char *out, size_t capacity) {
    size_t index = 0U;

    if ((out == (char *)0) || (capacity == 0U)) {
        return;
    }
    out[0] = '\0';
    if (source == (char const *)0) {
        return;
    }
    while (((index + 1U) < capacity) && (source[index] != '\0') &&
           (source[index] != '\r') && (source[index] != '\n')) {
        unsigned char character = (unsigned char)source[index];
        out[index] = ((character >= 0x20U) && (character < 0x7FU))
                         ? (char)character
                         : '.';
        ++index;
    }
    out[index] = '\0';
}

/** @brief 把 [start, end) 清洗成一行可打印 ASCII 并拷贝（含结尾 '\0'）。
* @return 写入的字符数（不含结尾 '\0'） */
static size_t copyClean(char *out, size_t capacity, char const *start, char const *end) {
    size_t index = 0U;

    if ((out == (char *)0) || (capacity == 0U)) {
        return 0U;
    }
    while ((start < end) && ((index + 1U) < capacity) && (*start != '\0')) {
        unsigned char const character = (unsigned char)*start;
        out[index] = ((character >= 0x20U) && (character < 0x7FU))
                         ? (char)character
                         : '.';
        ++index;
        ++start;
    }
    out[index] = '\0';
    return index;
}

/** @brief 取应答的「首行|末行」；两行相同时只打一次。
*
* NOTE (为什么不能只看首行，2026-09-27)：AT 应答是多行的——模组先回命令的确认
*   （`OK`），随后才用 **URC** 上报真正的结果。实例：手册（MN316 MQTT 用户手册
*   §2.2，第 10 页）写明 `AT+MQTTOPEN` 的命令响应是 `OK`，而连接成功/失败分别由
*   URC `+MQTTOPEN:OK` / `+MQTTOPEN:FAIL` 上报。原实现只取首行，于是上板日志变成
*   `rx="OK" reason=2`（超时）——把「模组明确报了连接失败」误读成「模组没说话」，
*   白等满 60 s 的 MQTT_OPEN 预算。首末都打出来，两种形态都不会漏。 */
static void describeReply(char const *source, char *out, size_t capacity) {
    char const *firstEnd;
    char const *lastStart;
    char const *lastEnd;
    char const *cursor;
    size_t used;

    if ((out == (char *)0) || (capacity == 0U)) {
        return;
    }
    out[0] = '\0';
    if (source == (char const *)0) {
        return;
    }

    for (firstEnd = source; (*firstEnd != '\0') && (*firstEnd != '\r') &&
                            (*firstEnd != '\n'); ++firstEnd) {
        /* 只找首行的结束位置 */
    }

    /* 末行：反复跳过行分隔符，最后一个非空行就是「模组的结论」所在。 */
    lastStart = source;
    lastEnd = firstEnd;
    cursor = firstEnd;
    while (*cursor != '\0') {
        while ((*cursor == '\r') || (*cursor == '\n')) {
            ++cursor;
        }
        if (*cursor == '\0') {
            break;
        }
        lastStart = cursor;
        while ((*cursor != '\0') && (*cursor != '\r') && (*cursor != '\n')) {
            ++cursor;
        }
        lastEnd = cursor;
    }

    used = copyClean(out, capacity, source, firstEnd);
    if ((lastStart != source) && ((used + 2U) < capacity)) {
        out[used] = '|';
        ++used;
        out[used] = '\0';
        (void)copyClean(&out[used], capacity - used, lastStart, lastEnd);
    }
}

/** @brief 打一条「第几步 / 哪条命令 / 模组回了什么 / 为什么没通过」的诊断 trace。
*  NOTE (为什么要它)：连接计划有十几个步骤，成功路径上一条日志都不打，上板时只能
*   看到「一直没动静」。这一行直接给出卡点：rx="" 说明模组一个字都没回（接线、供电
*   或波特率），rx="+CEREG: 2" 说明模组回了但值不对（未注册），reason=1 说明串口一直
*   写不出去。字段长度受限，超出会截断但不影响判读。
*  NOTE (rx 的形态)：`rx` 打的是应答的**首行|末行**（见 describeReply）。
*   例如 `rx="OK|+MQTTOPEN:FAIL"` = 命令被接受、但模组随后报了连接失败。
*  NOTE (只用于异常路径)：成功路径走 traceStepProgress()，只打 `step N/M ok`。
*   即「只有出错才把命令和应答刷出来」（2026-09-19 调整）。 */
static void traceStepResult(ModemComponent *me, char const *tag,
                            ModemFailure reason) {
    ModemBackend const *backend = Modem_Backend_Get();
    char command[MODEM_DIAG_FIELD_MAX];
    char reply[MODEM_DIAG_REPLY_MAX];
    char line[MODEM_DIAG_LINE_MAX];
    int produced;

    describeField(me->command, command, sizeof(command));
    describeReply(me->response, reply, sizeof(reply));
    produced = snprintf(line, sizeof(line),
                        "step %u/%u %s cmd=\"%s\" rx=\"%s\" reason=%u",
                        (unsigned)me->plan_step + 1U,
                        (unsigned)backend->plan_count, tag, command, reply,
                        (unsigned)reason);
    if (produced > 0) {
        MODEM_TRACE(me, line);
    }
}

/** @brief 在线期一条上行失败时的诊断：命令 + 应答 + reason。
*
* NOTE (为什么单独写一个，2026-09-20)：`traceStepResult()` 打的是「第 k/N 步」，而运行期
*   用的是计划步号（此时等于 plan_count），打出来会是 `step 21/20`、语义混乱；这里给
*   运行期单独的格式。
* NOTE (凭据)：运行期的命令只有上报报文与命令应答，**不含**用户名/密码（带凭据的
*   `AT+MQTTCFG` 属于连接计划步，走 traceStepResult），所以可以整条打出。
* NOTE (为什么要它)：实测那次「20 分钟后掉线」的导火索是运行期 reason=3，但原实现
*   在运行期失败路径上只打一行 `cloud link failed`，模组回的具体 CME 码全丢了。 */
static void traceRuntimeResult(ModemComponent *me, char const *tag,
                               ModemFailure reason) {
    char command[MODEM_DIAG_FIELD_MAX];
    char reply[MODEM_DIAG_REPLY_MAX];
    char line[MODEM_DIAG_LINE_MAX];
    int produced;

    describeField(me->command, command, sizeof(command));
    describeReply(me->response, reply, sizeof(reply));
    produced = snprintf(line, sizeof(line),
                        "runtime %s cmd=\"%s\" rx=\"%s\" reason=%u",
                        tag, command, reply, (unsigned)reason);
    if (produced > 0) {
        MODEM_TRACE(me, line);
    }
}

/** @brief 当前 TOPIC 步一共要下发多少条主题（用来把「第几条」写完整）。 */
static uint8_t topicTotal(ModemComponent const *me, ModemOp op) {
    uint8_t index;
    uint8_t total = 0U;

    for (index = 0U; index < (uint8_t)MODEM_TOPIC_COUNT; ++index) {
        ModemTopicSlot const *slot = &me->topic[index];
        if (!slot->registered) {
            continue;
        }
        if (((op == MODEM_OP_TOPIC_SUB) ? slot->subscribe_id
                                        : slot->publish_id) != 0U) {
            ++total;
        }
    }
    return total;
}

/** @brief 某一步**成功**时只打进度，不打命令与应答。
*  NOTE (与 traceStepResult 的分工，2026-09-19 按用户要求调整)：成功路径每一步都打
*   cmd/rx 会把串口刷满——连接计划 17 步，主题步还要按主题逐条展开，一次连接就是
*   二十多行；而排查时真正要看的只是「卡在第几步」。所以成功一律压成 `step N/M ok`，
*   命令与应答只在异常路径（wait / resend / recover / fail / write-fail）出现。
*  NOTE (为什么主题步要补 topic k/n)：主题步不推进 plan_step，只推进 topic_cursor，
*   只打 `step 14/17 ok` 会连着出现四行完全一样、看不出走到第几条。 */
static void traceStepProgress(ModemComponent *me) {
    ModemBackend const *backend = Modem_Backend_Get();
    ModemStep const *step = &backend->plan[me->plan_step];
    char line[MODEM_DIAG_LINE_MAX];
    int produced;

    if (step->mode == MODEM_STEP_TOPIC) {
        produced = snprintf(line, sizeof(line), "step %u/%u ok topic %u/%u",
                            (unsigned)me->plan_step + 1U,
                            (unsigned)backend->plan_count,
                            (unsigned)me->topic_cursor + 1U,
                            (unsigned)topicTotal(me, step->op));
    } else {
        produced = snprintf(line, sizeof(line), "step %u/%u ok",
                            (unsigned)me->plan_step + 1U,
                            (unsigned)backend->plan_count);
    }
    if (produced > 0) {
        MODEM_TRACE(me, line);
    }
}

/** @brief 连接开始时打一行总步数，给后续的 N/M 做参照。 */
static void tracePlanStart(ModemComponent *me) {
    ModemBackend const *backend = Modem_Backend_Get();
    char line[32];
    int produced = snprintf(line, sizeof(line), "connecting, %u steps",
                            (unsigned)backend->plan_count);

    MODEM_TRACE(me, (produced > 0) ? line : "connecting");
}
#else
/* trace 关掉时整块在编译期消失，调用点不用写 if。 */
#define traceStepResult(me_, tag_, reason_) ((void)0)
#define traceStepProgress(me_) ((void)0)
#define tracePlanStart(me_) ((void)0)
#define traceRuntimeResult(me_, tag_, reason_) ((void)0)
#endif

/** @brief 组装并写出当前步骤的命令；写出失败时只记重试时间，不阻塞。 */
static void sendStep(ModemComponent *me, ModemStep const *step, char const *expected,
                     uint32_t now) {
    size_t length = strlen(me->command);
    modem_port_status_t status;

    me->response_len = 0U;
    me->response[0] = '\0';
    me->attempt++;
    me->failure_step = me->plan_step;
    if ((length + 2U) >= sizeof(me->command)) {
        me->failure = MODEM_FAILURE_OVERFLOW;
        stepFailed(me, step, now);
        return;
    }
    me->command[length] = '\r';
    me->command[length + 1U] = '\n';
    status = me->port->write(me->port->context, (uint8_t const *)me->command,
                             length + 2U);
    if (status != MODEM_PORT_OK) {
        /* 串口忙或链路错：不把命令算作已发出，过 retry_ms 再发同一步。 */
        me->failure = MODEM_FAILURE_PORT_WRITE;
        me->next_attempt_ms = now + step->retry_ms;
        traceStepResult(me, "write-fail", MODEM_FAILURE_PORT_WRITE);
        return;
    }
    ModemAt_Begin(&me->at, expected, now, step->timeout_ms);
}

/** @brief 取计划中下一步；所有步骤都已完成时转入在线态。 */
static void startPlanStep(ModemComponent *me, uint32_t now) {
    ModemBackend const *backend = Modem_Backend_Get();
    char expected[MODEM_COMPONENT_TOKEN_MAX];

    for (;;) {
        ModemStep const *step;
        if (me->plan_step >= backend->plan_count) {
            finishPlan(me);
            return;
        }
        step = &backend->plan[me->plan_step];
        expected[0] = '\0';
        if (backend->build(me, me->plan_step, expected, sizeof(expected))) {
            me->state = stateOfOp(step->op);
            me->attempt = 0U;
            me->recover_done = false;
            me->step_started_ms = now;
            me->step_deadline_ms = now + ((step->mode == MODEM_STEP_REPEAT)
                                              ? step->budget_ms : step->timeout_ms);
            me->next_attempt_ms = now;
            sendStep(me, step, expected, now);
            return;
        }
        /* 这一步在当前配置下没有内容（例如某方向没有已登记主题） */
        ++me->plan_step;
        me->topic_cursor = 0U;
    }
}

/** @brief 当前步骤成功：TOPIC 步前进一条主题，其余前进整步。 */
static void stepSucceeded(ModemComponent *me, uint32_t now) {
    ModemBackend const *backend = Modem_Backend_Get();
    ModemStep const *step = &backend->plan[me->plan_step];

    traceStepProgress(me);
    ModemAt_Reset(&me->at);
    if (step->mode == MODEM_STEP_TOPIC) {
        ++me->topic_cursor;
    } else {
        ++me->plan_step;
        me->topic_cursor = 0U;
    }
    startPlanStep(me, now);
}

/** @brief 当前步骤失败：按模式继续、重试、补救或整条计划失败。 */
static void stepFailed(ModemComponent *me, ModemStep const *step, uint32_t now) {
    ModemBackend const *backend = Modem_Backend_Get();

    ModemAt_Reset(&me->at);
    if ((step->mode == MODEM_STEP_OPTIONAL) || (step->mode == MODEM_STEP_TOPIC)) {
        /* OPTIONAL：诊断类命令失败不影响连接；TOPIC：单条主题失败不拖垮整条链路。 */
        traceStepResult(me, "fail", me->failure);
        stepSucceeded(me, now);
        return;
    }
    if (step->mode == MODEM_STEP_REPEAT) {
        bool attempts_left = (me->attempt < step->attempts);
        bool budget_left = (step->budget_ms != 0U) &&
                           (!ModemAt_DeadlineReached(now, me->step_deadline_ms));
        if (attempts_left || budget_left) {
            /* NOTE (日志语义，2026-09-19)：轮询步「问到了但还没就绪」是会被重试的，
             *   不是失败。这里用 wait 而不是 fail —— 否则正常等待期（例如本后端的
             *   AT+RESET 之后模组要约 12 s 才建链，这段时间 AT+DTUSTATE 必然回 0）
             *   在日志里与真故障完全同形，2026-09-19 上板即因此被误判为连不上。
             *   reason 仍照实带出，用来区分「答了但值不对」(3) 与「压根没答」(2)。 */
            traceStepResult(me, "wait", me->failure);
            me->next_attempt_ms = now + step->retry_ms;
            return;
        }
        if ((backend->recover != (bool (*)(ModemComponent *, uint8_t))0) &&
            backend->recover(me, me->plan_step)) {
            traceStepResult(me, "recover", me->failure);
            return;   /* 补救已刷新期限，继续重试 */
        }
    }
    traceStepResult(me, "fail", me->failure);
    failPlan(me, me->failure);
}

/** @brief 连接阶段推进：在途命令判结果，没有在途命令就发下一步。 */
static void connectLoop(ModemComponent *me, uint32_t now) {
    ModemBackend const *backend = Modem_Backend_Get();
    ModemStep const *step;
    ModemAtResult result;
    char expected[MODEM_COMPONENT_TOKEN_MAX];

    if (me->plan_step >= backend->plan_count) {
        finishPlan(me);
        return;
    }
    step = &backend->plan[me->plan_step];

    /* NOTE (修复，2026-09-19)：结论不能只在 phase == WAITING 时取。
     * 应答是经 readPort → feedLine → ModemAt_Feed 喂进引擎的，而 Feed 一旦命中期望标记
     * 或错误码就把 phase 置回 IDLE；等本函数看结果时 phase 已经不是 WAITING，结论就被整条
     * 丢掉：组件以为命令从未完成，于是不停重发同一条命令（2026-09-19 上板实测：模组已回
     * OK 却每 260 ms 重发一次、plan 停在 0）。
     * NOTE (坑)：命令在途（Tick 仍返回 PENDING）时必须就此返回等待。第一版改这里时漏了
     * 这一条，代码会掉到下面的重发路径，变成每一拍都重发一次（实测 20 ms 一次、try 每
     * 2 秒涨 100）。「等」与「重发」必须严格分开。 */
    result = me->at.result;
    if ((result == MODEM_AT_PENDING) && (me->at.phase == MODEM_AT_WAITING)) {
        result = ModemAt_Tick(&me->at, now);
        if (result == MODEM_AT_PENDING) {
            return;
        }
    }
    if (result != MODEM_AT_PENDING) {
        if ((result == MODEM_AT_DONE) && backend->apply(me, me->plan_step)) {
            stepSucceeded(me, now);
            return;
        }
        /* NOTE (可容忍错误，2026-09-20 新增)：有些命令的「失败」正是期望状态——
         *   MN316 上没有 MQTT 客户端时 `AT+MQTTDEL` 必然回 `+CME ERROR:8002`，而
         *   「没有客户端」恰恰是这一步要的结果。计划表用 step->tolerate 声明这种串，
         *   核心在收到 MODEM_AT_ERROR 且应答文本里确实出现该串时按成功处理。
         *   NOTE (为什么必须在这里判)：错误行会被 ModemAt_Feed 当场判成 ERROR
         *   （modem_at.c:87 错误优先），上面的 apply() 分支只在 DONE 时才走，
         *   后端根本没有机会说「这个错误我接受」。 */
        if ((result == MODEM_AT_ERROR) && (step->tolerate != (char const *)0) &&
            (step->tolerate[0] != '\0') &&
            (strstr(me->response, step->tolerate) != (char const *)0)) {
            stepSucceeded(me, now);
            return;
        }
        me->failure = (result == MODEM_AT_DONE) ? MODEM_FAILURE_RESPONSE
                     : (result == MODEM_AT_ERROR) ? MODEM_FAILURE_RESPONSE
                                                  : MODEM_FAILURE_TIMEOUT;
        stepFailed(me, step, now);
        return;
    }

    /* 没有在途命令：要么上一条写不出去，要么 REPEAT 步到了重发时间。 */
    if (!ModemAt_DeadlineReached(now, me->next_attempt_ms)) {
        return;
    }
    /* NOTE (修复，2026-09-19)：这里原先写成 (step->mode != MODEM_STEP_REPEAT)，把等待型
     * 步骤排除在期限判定之外。但超期判定的另一条路（ModemAt_Tick）只在「命令在途」
     * （phase == WAITING）时才有效；一旦某一步反复走「没有在途命令」这条路——串口写不
     * 出去，或者应答恰好在期限边界上被 ModemAt_Feed 内部的判定吃掉——两条路就同时断开：
     * 该步每 retry_ms 重发一次、永不超期、也永不交给上层重连。2026-09-19 上板实测就卡成
     * 了这样（探活 20 秒内只报一次超时，之后再无失败、也无重连）。等待型步骤同样必须受
     * 自己的预算约束，所以去掉模式豁免，让超期统一走 stepFailed。 */
    /* NOTE (修复，2026-09-20)：原先是「attempt > 0 且期限已到」就判超期。但
     *   budget_ms == 0 的 REPEAT 步（MN316 的 MQTTDEL / MQTTCFG）在 startPlanStep
     *   里把 step_deadline_ms 设成 now + 0，即一设就已到期，于是每次到点都从这里
     *   stepFailed，**永远走不到下面的重发分支**：sendStep() 不被调用 → attempt 永不
     *   递增 → attempts_left 恒为真 → 每 retry_ms 打一行 wait，既不推进、也不失败、
     *   也不通知上层重连（2026-09-20 上板实测：卡在 step 15、每 500 ms 一行同样的
     *   `wait cmd="AT+MQTTDEL"`）。
     *   现在改成「次数与预算都用尽」才判超期，与 stepFailed() 的口径一致。有预算的
     *   轮询步（attempts=1、budget>0）不受影响——它们的次数在首拍就用尽，仍由
     *   step_deadline_ms 决定何时放弃。 */
    if ((me->attempt >= step->attempts) &&
        ((step->budget_ms == 0U) || ModemAt_DeadlineReached(now, me->step_deadline_ms))) {
        stepFailed(me, step, now);
        return;
    }
    expected[0] = '\0';
    if (!backend->build(me, me->plan_step, expected, sizeof(expected))) {
        ++me->plan_step;
        me->topic_cursor = 0U;
        startPlanStep(me, now);
        return;
    }
    traceStepResult(me, "resend", MODEM_FAILURE_NONE);
    sendStep(me, step, expected, now);
}

/*--------------------------------------------------------------- 上报报文 */
/** @brief 用采样数据组装 data 报文；返回 false 表示报文放不下。 */
static bool buildReport(ModemComponent *me) {
    size_t used;
    int produced = snprintf(me->payload, sizeof(me->payload),
                            "{\"cmd\":\"data\",\"seq\":%lu",
                            (unsigned long)me->sequence);

    if ((produced < 0) || ((size_t)produced >= sizeof(me->payload))) {
        return false;
    }
    used = (size_t)produced;
    if (!appendDecimal(me->payload, sizeof(me->payload), &used, "temperature",
                       me->data.temperature_cC, 2U)) {
        return false;
    }
    if (!appendDecimal(me->payload, sizeof(me->payload), &used, "humidity",
                       (int32_t)me->data.humidity_permil, 1U)) {
        return false;
    }
    if (!appendInteger(me->payload, sizeof(me->payload), &used, "signal_quality",
                       me->data.signal_quality)) {
        return false;
    }
    /* 取不到的字段整条跳过：宁可报文里没有这个键，也不要报一个假值。 */
    if ((me->data.tac[0] != '\0') &&
        (!appendText(me->payload, sizeof(me->payload), &used, "cell_tac",
                     me->data.tac))) {
        return false;
    }
    if ((me->data.cell_id[0] != '\0') &&
        (!appendText(me->payload, sizeof(me->payload), &used, "cell_id",
                     me->data.cell_id))) {
        return false;
    }
    if (me->data.location_valid) {
        if (!appendDecimal(me->payload, sizeof(me->payload), &used, "location_lon",
                           me->data.lon_e6, 6U)) {
            return false;
        }
        if (!appendDecimal(me->payload, sizeof(me->payload), &used, "location_lat",
                           me->data.lat_e6, 6U)) {
            return false;
        }
    }
    return append(me->payload, sizeof(me->payload), &used, "}");
}

/*--------------------------------------------------------------- 读写属性 */
/** @brief 按字段名格式化当前值；未知字段返回 false。 */
static bool formatReadField(ModemComponent *me, char const *name,
                            char *out, size_t capacity) {
    if (strcmp(name, "temperature") == 0) {
        return formatDecimal(out, capacity, me->data.temperature_cC, 2U);
    }
    if (strcmp(name, "humidity") == 0) {
        return formatDecimal(out, capacity, (int32_t)me->data.humidity_permil, 1U);
    }
    if (strcmp(name, "signal_quality") == 0) {
        return formatInteger(out, capacity, me->data.signal_quality);
    }
    if (strcmp(name, "cell_tac") == 0) {
        return (me->data.tac[0] != '\0') && formatText(out, capacity, me->data.tac);
    }
    if (strcmp(name, "cell_id") == 0) {
        return (me->data.cell_id[0] != '\0') &&
               formatText(out, capacity, me->data.cell_id);
    }
    if (strcmp(name, "location_lon") == 0) {
        return me->data.location_valid &&
               formatDecimal(out, capacity, me->data.lon_e6, 6U);
    }
    if (strcmp(name, "location_lat") == 0) {
        return me->data.location_valid &&
               formatDecimal(out, capacity, me->data.lat_e6, 6U);
    }
    if (strcmp(name, "sample_interval") == 0) {
        return formatInteger(out, capacity, (int32_t)me->sample_interval_min);
    }
    if (strcmp(name, "report_enabled") == 0) {
        return formatText(out, capacity, me->report_enabled ? "true" : "false");
    }
    return false;
}

/** @brief 组装 get 应答；字段不存在时也给出 ok:false 应答。 */
static bool buildGetReply(ModemComponent *me, char const *name,
                          char *out, size_t capacity) {
    char text[32];
    int produced;

    if (formatReadField(me, name, text, sizeof(text))) {
        produced = snprintf(out, capacity,
                            "{\"cmd\":\"get\",\"param\":\"%s\",\"ok\":true,\"v\":%s}",
                            name, text);
    } else {
        produced = snprintf(out, capacity,
                            "{\"cmd\":\"get\",\"param\":\"%s\",\"ok\":false,\"err\":1}",
                            name);
    }
    return (produced >= 0) && ((size_t)produced < capacity);
}

/** @brief 写入一项可写属性并组装 set 应答；返回 false 表示应答放不下。 */
static bool buildSetReply(ModemComponent *me, char const *name, char const *value,
                          char *out, size_t capacity) {
    char text[32];
    int32_t numeric = 0;
    int produced;
    bool accepted = false;

    if (strcmp(name, "sample_interval") == 0) {
        if (!parseInt32(value, &numeric)) {
            produced = snprintf(out, capacity,
                                "{\"cmd\":\"set\",\"param\":\"%s\",\"ok\":false,\"err\":4}",
                                name);
            return (produced >= 0) && ((size_t)produced < capacity);
        }
        if ((numeric < (int32_t)MODEM_COMPONENT_SAMPLE_MIN) ||
            (numeric > (int32_t)MODEM_COMPONENT_SAMPLE_MAX)) {
            produced = snprintf(out, capacity,
                                "{\"cmd\":\"set\",\"param\":\"%s\",\"ok\":false,\"err\":2}",
                                name);
            return (produced >= 0) && ((size_t)produced < capacity);
        }
        me->sample_interval_min = (uint32_t)numeric;
        accepted = true;
    } else if (strcmp(name, "report_enabled") == 0) {
        if (strcmp(value, "true") == 0) {
            me->report_enabled = true;
            accepted = true;
        } else if (strcmp(value, "false") == 0) {
            me->report_enabled = false;
            accepted = true;
        } else {
            produced = snprintf(out, capacity,
                                "{\"cmd\":\"set\",\"param\":\"%s\",\"ok\":false,\"err\":4}",
                                name);
            return (produced >= 0) && ((size_t)produced < capacity);
        }
    } else {
        produced = snprintf(out, capacity,
                            "{\"cmd\":\"set\",\"param\":\"%s\",\"ok\":false,\"err\":1}",
                            name);
        return (produced >= 0) && ((size_t)produced < capacity);
    }

    if (accepted && !formatReadField(me, name, text, sizeof(text))) {
        return false;
    }
    produced = snprintf(out, capacity,
                        "{\"cmd\":\"set\",\"param\":\"%s\",\"ok\":true,\"err\":0,\"v\":%s}",
                        name, text);
    return (produced >= 0) && ((size_t)produced < capacity);
}

/*--------------------------------------------------------------- 下行分发 */
/** @brief 把一条应答排进上行队列（同一时刻只保留最新一条）。 */
static void queueReply(ModemComponent *me, ModemTopic topic, char const *message,
                       uint8_t qos) {
    size_t length = strlen(message);

    if ((length + 1U) > sizeof(me->reply_payload)) {
        MODEM_TRACE(me, "reply too long; dropped");
        return;
    }
    (void)memcpy(me->reply_payload, message, length + 1U);
    me->reply_topic = topic;
    me->reply_qos = qos;
    me->reply_pending = true;
}

/** @brief 按逻辑主题与 cmd 字段处理 ping / get / set。 */
static void dispatchDownlink(ModemComponent *me, ModemTopic topic) {
    char command[16];
    char parameter[40];
    char value[32];

    if (!jsonString(me->payload, "cmd", command, sizeof(command))) {
        return;
    }
    if ((topic == MODEM_TOPIC_STATE) && (strcmp(command, "ping") == 0)) {
        queueReply(me, MODEM_TOPIC_STATE_REPLY, "{\"cmd\":\"pong\"}", 1U);
    } else if ((topic == MODEM_TOPIC_READ) && (strcmp(command, "get") == 0) &&
               jsonString(me->payload, "param", parameter, sizeof(parameter)) &&
               buildGetReply(me, parameter, me->payload, sizeof(me->payload))) {
        queueReply(me, MODEM_TOPIC_READ_REPLY, me->payload, 1U);
    } else if ((topic == MODEM_TOPIC_WRITE) && (strcmp(command, "set") == 0) &&
               jsonString(me->payload, "param", parameter, sizeof(parameter)) &&
               jsonValue(me->payload, "v", value, sizeof(value)) &&
               buildSetReply(me, parameter, value, me->payload,
                             sizeof(me->payload))) {
        queueReply(me, MODEM_TOPIC_WRITE_REPLY, me->payload, 1U);
    } else {
        /* function/invoke 与未知命令：当前版本不响应，见改动日志「遗留」。 */
        MODEM_TRACE(me, "downlink is not handled");
    }
}

/** @brief 取走一条已收全的通知并分发；无论成败都清空缓冲，避免卡死链路。 */
static void handleDownlink(ModemComponent *me) {
    ModemBackend const *backend = Modem_Backend_Get();
    ModemTopic topic = MODEM_TOPIC_REPORT;

    if (backend->urc_extract(me, me->urc, me->urc_len, &topic, me->payload,
                             sizeof(me->payload))) {
        dispatchDownlink(me, topic);
    } else {
        MODEM_TRACE(me, "downlink dropped (unparsable or unregistered topic)");
    }
    me->urc_len = 0U;
    me->urc[0] = '\0';
    me->urc_ready = false;
}

/*--------------------------------------------------------------- 在线期推进 */
/** @brief 起一条运行期命令（应答发布 / 数据上报 / 小区查询）。 */
static void startRuntime(ModemComponent *me, ModemRuntimeOp op, uint32_t now) {
    ModemBackend const *backend = Modem_Backend_Get();
    char expected[MODEM_COMPONENT_TOKEN_MAX];
    char const *payload = (char const *)0;
    ModemTopic topic = MODEM_TOPIC_REPORT;
    uint32_t timeout_ms = MODEM_COMPONENT_TIMEOUT_MS;

    expected[0] = '\0';
    switch (op) {
        case MODEM_RUNTIME_REPLY:
            payload = me->reply_payload;
            topic = me->reply_topic;
            if (!backend->publish_build(me, topic, payload, expected,
                                        sizeof(expected))) {
                me->reply_pending = false;
                return;
            }
            break;
        case MODEM_RUNTIME_REPORT:
            if (!buildReport(me)) {
                MODEM_TRACE(me, "report build overflow");
                me->report_requested = false;
                return;
            }
            if (!backend->publish_build(me, MODEM_TOPIC_REPORT, me->payload, expected,
                                        sizeof(expected))) {
                me->report_requested = false;
                return;
            }
            break;
        case MODEM_RUNTIME_CELL:
            if (!backend->cell_build(me, expected, sizeof(expected))) {
                me->cell_requested = false;
                return;
            }
            timeout_ms = MODEM_COMPONENT_CONFIG_TIMEOUT_MS;
            break;
        case MODEM_RUNTIME_LOCATION:
            /* NOTE (静默丢弃)：后端没实现定位时直接清掉请求，不发命令、也不打 trace——
             *   上层是按固定周期请求的，每次打一条会把日志刷满。
             * NOTE (两个都要校验)：finishRuntime() 会无条件调 location_apply，所以这里
             *   必须把「两个钩子都非空」作为受理条件，否则后端只填一个就会在应答到达时
             *   调到空指针。只填一个视为不支持。 */
            if ((backend->location_build == (bool (*)(ModemComponent *, char *, size_t))0) ||
                (backend->location_apply == (bool (*)(ModemComponent *))0) ||
                (!backend->location_build(me, expected, sizeof(expected)))) {
                me->location_requested = false;
                return;
            }
            /* NOTE (超时给够)：ML307C 的 AT+LBS 在模组侧最长要 5 s 才出结果（手册明示），
             *   因此沿用配置类那档超时，不能用查询类那档很短的超时。 */
            timeout_ms = MODEM_COMPONENT_CONFIG_TIMEOUT_MS;
            break;
        default:
            return;
    }
    me->runtime_op = op;
    me->failure_step = me->plan_step;
    me->response_len = 0U;
    me->response[0] = '\0';
    {
        size_t length = strlen(me->command);
        if ((length + 2U) >= sizeof(me->command)) {
            me->failure = MODEM_FAILURE_OVERFLOW;
            me->runtime_op = MODEM_RUNTIME_NONE;
            failPlan(me, MODEM_FAILURE_OVERFLOW);
            return;
        }
        me->command[length] = '\r';
        me->command[length + 1U] = '\n';
        if (me->port->write(me->port->context, (uint8_t const *)me->command,
                            length + 2U) != MODEM_PORT_OK) {
            /* 串口忙：保留请求，下一个 Process 周期再试。 */
            me->runtime_op = MODEM_RUNTIME_NONE;
            return;
        }
    }
    ModemAt_Begin(&me->at, expected, now, timeout_ms);
}

/** @brief 收尾一条运行期命令。 */
static void finishRuntime(ModemComponent *me, ModemAtResult result) {
    ModemBackend const *backend = Modem_Backend_Get();
    ModemRuntimeOp op = me->runtime_op;

    me->runtime_op = MODEM_RUNTIME_NONE;
    ModemAt_Reset(&me->at);
    if (result != MODEM_AT_DONE) {
        me->failure = (result == MODEM_AT_ERROR) ? MODEM_FAILURE_RESPONSE
                                                 : MODEM_FAILURE_TIMEOUT;
        if (op == MODEM_RUNTIME_CELL) {
            /* 小区信息是尽力而为的补充数据，查不到不影响在线状态。 */
            me->cell_requested = false;
            MODEM_TRACE(me, "cell info unavailable");
            return;
        }
        if (op == MODEM_RUNTIME_LOCATION) {
            /* NOTE (尽力而为)：定位失败是常态——室内或基站稀疏时模组直接回错误码
             *   （ML307C 为 +CME ERROR: 130），手册自己的示例就是失败。这里只清请求
             *   标志：不动连接状态，也不清掉上一次取到的有效坐标。 */
            me->location_requested = false;
            MODEM_TRACE(me, "location unavailable");
            return;
        }
        /* NOTE (诊断，2026-09-20)：运行期失败原先只打 "cloud link failed" 一行，
         *   模组回的具体错误码全丢（实测那次 20 分钟掉线的导火索就这样丢了现场）。
         *   这里补上命令与应答。 */
        traceRuntimeResult(me, "fail", me->failure);
        /* NOTE (上行重试，2026-09-20)：一条上行走不掉**不再**直接毁掉整条链路。
         *   实测：NB-IoT 上一次上行抖动就让设备在 20 分钟后进重连阶梯，又因模组掉网
         *   后不再自己搜网而最终永久沉默。现在连续 MODEM_COMPONENT_UPLINK_RETRY 次
         *   都发不出去才判链路失效；重试之间按 UPLINK_RETRY_MS 退避（否则三次会在
         *   几十毫秒内用光，等于没重试）。 */
        if (((op == MODEM_RUNTIME_REPORT) || (op == MODEM_RUNTIME_REPLY)) &&
            (me->uplink_retry < MODEM_COMPONENT_UPLINK_RETRY)) {
            uint32_t retryNow = me->port->get_tick_ms(me->port->context);
            ++me->uplink_retry;
            if (op == MODEM_RUNTIME_REPORT) {
                me->report_requested = true;
            }
            else {
                me->reply_pending = true;
            }
            me->uplink_retry_at_ms = retryNow + MODEM_COMPONENT_UPLINK_RETRY_MS;
            MODEM_TRACE(me, "uplink failed, retry scheduled");
            return;
        }
        /* 重试用尽：链路确实不可用，转入失败态交给上层重连。 */
        failPlan(me, me->failure);
        return;
    }
    me->uplink_retry = 0U;   /* 上行成功：清掉连续失败计数 */
    switch (op) {
        case MODEM_RUNTIME_REPLY:
            me->reply_pending = false;
            break;
        case MODEM_RUNTIME_REPORT:
            ++me->sequence;
            me->report_requested = false;
            break;
        case MODEM_RUNTIME_CELL:
            (void)backend->cell_apply(me);
            me->cell_requested = false;
            break;
        case MODEM_RUNTIME_LOCATION:
            /* 解析失败也只是这一次没拿到坐标，同样不影响在线状态。 */
            if (!backend->location_apply(me)) {
                MODEM_TRACE(me, "location parse failed");
            }
            me->location_requested = false;
            break;
        default:
            break;
    }
}

/** @brief 在线期推进：先消化下行，再按需起一条上行命令。 */
static void readyLoop(ModemComponent *me, uint32_t now) {
    ModemAtResult result;

    /* NOTE (修复，2026-09-19)：同 connectLoop——上行应答同样是经 Feed 命中后把 phase
     * 置回 IDLE 的，只看 WAITING 会丢掉结论，把「已经发出去」当成没发，导致上报或命令应答
     * 被反复重发。同样注意：命令在途时必须返回等待，不能掉到下面「起一条新命令」的路径。 */
    result = me->at.result;
    if ((result == MODEM_AT_PENDING) && (me->at.phase == MODEM_AT_WAITING)) {
        result = ModemAt_Tick(&me->at, now);
        if (result == MODEM_AT_PENDING) {
            return;
        }
    }
    if (result != MODEM_AT_PENDING) {
        finishRuntime(me, result);
        return;
    }
    if (me->urc_ready) {
        handleDownlink(me);
        return;
    }
    /* NOTE (上行重试节流，2026-09-20)：失败的上行按 UPLINK_RETRY_MS 退避后再发。
     *   这里对整段运行期命令生效（上报、应答、小区、定位都让路）——它们本来就是
     *   周期性的，晚几秒没有代价，而提前重发只会把重试次数白白用光。 */
    if (!ModemAt_DeadlineReached(now, me->uplink_retry_at_ms)) {
        return;
    }
    if (me->reply_pending) {
        startRuntime(me, MODEM_RUNTIME_REPLY, now);
    } else if (me->report_requested) {
        startRuntime(me, MODEM_RUNTIME_REPORT, now);
    } else if (me->cell_requested) {
        startRuntime(me, MODEM_RUNTIME_CELL, now);
    } else if (me->location_requested) {
        /* 定位排在最后：它最长要占 5 s（模组侧），不该挡在上报与命令应答前面。 */
        startRuntime(me, MODEM_RUNTIME_LOCATION, now);
    }
}

/*--------------------------------------------------------------- 公共接口 */
/** @brief 取当前编译进固件的后端实现表。
* @note 后端选择在 modem_backend.h 里已用 #if/#error 校验过，这里只需取地址。 */
ModemBackend const *Modem_Backend_Get(void) {
#if (MODEM_COMPONENT_BACKEND == MODEM_BACKEND_MN316)
    return &Modem_Mn316_Backend;
#elif (MODEM_COMPONENT_BACKEND == MODEM_BACKEND_NT26)
    return &Modem_Nt26_Backend;
#else
    return &Modem_Ml307c_Backend;   /* 取值合法性由 modem_backend.h 的 #error 保证 */
#endif
}

void Modem_Component_Init(ModemComponent *me, ModemPort const *port,
                          ModemConfig const *config) {
    if ((me == (ModemComponent *)0) || (port == (ModemPort const *)0) ||
        (config == (ModemConfig const *)0)) {
        return;
    }
    (void)memset(me, 0, sizeof(*me));
    me->port = port;
    me->config = config;
    me->state = MODEM_COMPONENT_IDLE;
    me->failure = MODEM_FAILURE_NONE;
    me->report_enabled = true;
    me->sample_interval_min = MODEM_COMPONENT_SAMPLE_MIN;
    me->report_period_ms = (uint32_t)config->report_period_s * 1000U;
    me->cell_period_ms = (uint32_t)MODEM_COMPONENT_CELL_PERIOD_S * 1000U;
    ModemAt_Init(&me->at);
    buildTopics(me);
}

void Modem_Component_Start(ModemComponent *me) {
    if ((me == (ModemComponent *)0) || (me->port == (ModemPort const *)0) ||
        (me->config == (ModemConfig const *)0)) {
        return;
    }
    ModemAt_Reset(&me->at);
    buildTopics(me);
    me->plan_step = 0U;
    me->topic_cursor = 0U;
    me->attempt = 0U;
    me->recover_done = false;
    me->next_attempt_ms = 0U;
    me->step_started_ms = 0U;
    me->step_deadline_ms = 0U;
    me->response_len = 0U;
    me->response[0] = '\0';
    me->line_len = 0U;
    me->line_overflow = false;
    me->urc_len = 0U;
    me->urc[0] = '\0';
    me->urc_ready = false;
    me->reply_pending = false;
    me->runtime_op = MODEM_RUNTIME_NONE;
    me->report_requested = false;
    me->cell_requested = false;
    me->location_requested = false;
    me->uplink_retry = 0U;
    me->uplink_retry_at_ms = 0U;
    me->recover_phase = 0U;
    me->failure = MODEM_FAILURE_NONE;
    me->failure_step = 0U;
    me->sequence = 0U;
    me->state = MODEM_COMPONENT_PROBING;
    tracePlanStart(me);
    startPlanStep(me, nowMs(me));
}

void Modem_Component_Process(ModemComponent *me) {
    uint32_t now;

    if ((me == (ModemComponent *)0) || (me->port == (ModemPort const *)0) ||
        (me->config == (ModemConfig const *)0)) {
        return;
    }
    if (me->state == MODEM_COMPONENT_IDLE) {
        return;
    }
    now = nowMs(me);
    readPort(me, now);   /* 先把串口已有字节吃掉：喂行会直接改变 AT 引擎结果 */
    if (me->state == MODEM_COMPONENT_READY) {
        readyLoop(me, now);
    } else if (me->state != MODEM_COMPONENT_FAILED) {
        connectLoop(me, now);
    } else {
        /* 失败态不再产生流量，等上层调 Modem_Component_Start() 重连。 */
    }
}

void Modem_Component_RequestReport(ModemComponent *me) {
    if (me != (ModemComponent *)0) {
        me->report_requested = true;
    }
}

void Modem_Component_RequestCellInfo(ModemComponent *me) {
    if (me != (ModemComponent *)0) {
        me->cell_requested = true;
    }
}

void Modem_Component_RequestLocation(ModemComponent *me) {
    if (me != (ModemComponent *)0) {
        me->location_requested = true;
    }
}

bool Modem_Component_IsReady(ModemComponent const *me) {
    return (me != (ModemComponent const *)0) &&
           (me->state == MODEM_COMPONENT_READY);
}
