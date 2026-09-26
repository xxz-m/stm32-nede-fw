/*============================================================================
* modem_at.c —— 芯片无关的增量式 AT 引擎实现
* 层次定位：Component 层内部实现；只依赖标准 C，不包含 HAL/QP/RTOS。
* 来源    ：由 E:\stm32\stm32\Drivers\Modem\Src\modem_at.c（阻塞版）重写为
*           非阻塞的「喂行 + 查期限」模型，命令拼装与错误识别规则保持不变。
*===========================================================================*/
#include "modem_at.h"

#include <string.h>

/** @brief 复位引擎到空闲态。 */
void ModemAt_Init(ModemAt *me) {
    if (me == (ModemAt *)0) {
        return;
    }
    me->phase = MODEM_AT_IDLE;
    me->result = MODEM_AT_PENDING;
    me->expected[0] = '\0';
    me->deadline_ms = 0U;
}

/** @brief 丢弃当前命令回到空闲态。 */
void ModemAt_Reset(ModemAt *me) {
    if (me == (ModemAt *)0) {
        return;
    }
    me->phase = MODEM_AT_IDLE;
    me->result = MODEM_AT_PENDING;
    me->expected[0] = '\0';
    me->deadline_ms = 0U;
}

/** @brief 记下一次「命令已写出」，进入等待应答状态。 */
void ModemAt_Begin(ModemAt *me, char const *expected, uint32_t now_ms, uint32_t timeout_ms) {
    size_t length;

    if (me == (ModemAt *)0) {
        return;
    }
    if (timeout_ms == 0U) {
        timeout_ms = MODEM_COMPONENT_TIMEOUT_MS;
    }
    me->expected[0] = '\0';
    if (expected != (char const *)0) {
        length = strlen(expected);
        if (length >= sizeof(me->expected)) {
            /* 期望标记放不下就退化成「只等 OK」：宁可少匹配，也不要越界。 */
            length = sizeof(me->expected) - 1U;
        }
        (void)memcpy(me->expected, expected, length);
        me->expected[length] = '\0';
    }
    me->phase = MODEM_AT_WAITING;
    me->result = MODEM_AT_PENDING;
    me->deadline_ms = now_ms + timeout_ms;
}

/** @brief 判断一行文本是否为 AT 通用错误行。 */
bool ModemAt_LineHasError(char const *line) {
    if (line == (char const *)0) {
        return false;
    }
    /* NOTE (`:FAIL` 形态，2026-09-27)：本模组除了 `ERROR` / `+CME ERROR:` 之外，
    *   还用**自定义 URC** 上报失败，形如 `+<命令>:FAIL`。手册（MN316 MQTT 用户手册
    *   §2.2，第 10 页）写明：`AT+MQTTOPEN` 的命令响应是 `OK`，而连接成功/失败分别由
    *   URC `+MQTTOPEN:OK` / **`+MQTTOPEN:FAIL`** 上报。
    *   原先只认 `ERROR` / `+CME ERROR:` / `+CMS ERROR:`，于是 `+MQTTOPEN:FAIL`
    *   既不算错误、也不匹配期望标记，被引擎**当成噪声忽略**，只能等满 60 s 的超时
    *   （上板实测：`step 18/21 fail … rx="OK" reason=2`，把「模组明确报了连接失败」
    *   掩盖成「模组没说话」）。这里把 `:FAIL` 一并按错误行处理——`+XXX:FAIL` 是 AT
    *   的通用上报形式，这样以后别的命令用同样形态也不需要再改。 */
    return (strstr(line, "ERROR") != (char *)0) ||
           (strstr(line, "+CME ERROR:") != (char *)0) ||
           (strstr(line, "+CMS ERROR:") != (char *)0) ||
           (strstr(line, ":FAIL") != (char *)0);
}

/** @brief 判断一行文本是否含指定应答标记。 */
bool ModemAt_LineHasToken(char const *line, char const *token) {
    if ((line == (char const *)0) || (token == (char const *)0) || (*token == '\0')) {
        return false;
    }
    return (strstr(line, token) != (char *)0);
}

/** @brief 喂入一整行；判定错误、命中期望标记或继续等待。 */
ModemAtResult ModemAt_Feed(ModemAt *me, char const *line, size_t length, uint32_t now_ms) {
    if (me == (ModemAt *)0) {
        return MODEM_AT_ERROR;
    }
    if (me->phase != MODEM_AT_WAITING) {
        return me->result;
    }
    (void)length;

    /* 错误优先判定：一条应答里同时出现错误码和 OK 时，按失败处理更安全。 */
    if (ModemAt_LineHasError(line)) {
        me->result = MODEM_AT_ERROR;
        me->phase = MODEM_AT_IDLE;
        return me->result;
    }
    if (ModemAt_LineHasToken(line, me->expected)) {
        me->result = MODEM_AT_DONE;
        me->phase = MODEM_AT_IDLE;
        return me->result;
    }
    /* 还没结论：顺手做一次超期判定，避免调用方忘记调 Tick。 */
    return ModemAt_Tick(me, now_ms);
}

/** @brief 只推进超时判定。 */
ModemAtResult ModemAt_Tick(ModemAt *me, uint32_t now_ms) {
    if (me == (ModemAt *)0) {
        return MODEM_AT_ERROR;
    }
    if ((me->phase == MODEM_AT_WAITING) &&
        ModemAt_DeadlineReached(now_ms, me->deadline_ms)) {
        me->result = MODEM_AT_TIMEOUT;
        me->phase = MODEM_AT_IDLE;
    }
    return me->result;
}

/** @brief 从游标处取下一个十进制整数。 */
bool ModemAt_NextInt(char const **cursor, int32_t *out) {
    char const *text;

    if ((cursor == (char const **)0) || (*cursor == (char const *)0) ||
        (out == (int32_t *)0)) {
        return false;
    }
    text = *cursor;
    while ((*text != '\0') && ((*text < '0') || (*text > '9'))) {
        ++text;
    }
    if (*text == '\0') {
        *cursor = text;
        return false;
    }
    *out = 0;
    while ((*text >= '0') && (*text <= '9')) {
        *out = (*out * 10) + (int32_t)(*text - '0');
        ++text;
    }
    *cursor = text;
    return true;
}

/** @brief 从游标处取下一个双引号字段。 */
bool ModemAt_NextQuoted(char const **cursor, char *out, size_t capacity) {
    char const *start;
    char const *end;
    size_t length;

    if ((cursor == (char const **)0) || (*cursor == (char const *)0) ||
        (out == (char *)0) || (capacity < 2U)) {
        return false;
    }
    start = strchr(*cursor, '"');
    if (start == (char const *)0) {
        return false;
    }
    ++start;
    end = strchr(start, '"');
    if (end == (char const *)0) {
        return false;
    }
    length = (size_t)(end - start);
    if ((length == 0U) || ((length + 1U) > capacity)) {
        *cursor = end;
        return false;
    }
    (void)memcpy(out, start, length);
    out[length] = '\0';
    *cursor = end + 1;
    return true;
}

/** @brief 取下一个字段：看首个非空白字符是不是引号，决定按引号读还是按裸文本读。
* NOTE (为什么要跳过前导逗号)：本引擎的 NextInt() 读完数字后把游标停在**分隔符上**
*   （`+CEREG:2,1,6E55,...` 读掉两个整数后游标指向第 3 个字段前的那个逗号）。
*   若这里只跳空格，裸文本分支会立刻撞上 ',' 得到空字段而返回 false。
*   与 NextQuoted()（内部 strchr('"')，天然跳过一切）保持同样的宽容度。 */
bool ModemAt_NextField(char const **cursor, char *out, size_t capacity) {
    char const *probe;
    char const *end;
    size_t length;

    if ((cursor == (char const **)0) || (*cursor == (char const *)0) ||
        (out == (char *)0) || (capacity < 2U)) {
        return false;
    }
    probe = *cursor;
    while ((*probe == ' ') || (*probe == '\t') || (*probe == ',')) {
        ++probe;
    }
    if (*probe == '"') {
        return ModemAt_NextQuoted(cursor, out, capacity);
    }
    /* 不带引号：读到 ',' / 行尾 / 串尾为止。注意不能顺手把 CRLF 吃进来。 */
    end = probe;
    while ((*end != '\0') && (*end != ',') && (*end != '\r') && (*end != '\n')) {
        ++end;
    }
    while ((end > probe) && ((end[-1] == ' ') || (end[-1] == '\t'))) {
        --end;                       /* 去掉尾部空格 */
    }
    length = (size_t)(end - probe);
    *cursor = (*end == ',') ? (end + 1) : end;
    if ((length == 0U) || ((length + 1U) > capacity)) {
        return false;
    }
    (void)memcpy(out, probe, length);
    out[length] = '\0';
    return true;
}

/** @brief 把定点文本按 1e-6 解析成整数。 */
bool ModemAt_ParseE6(char const *text, int32_t *out) {
    int32_t whole = 0;
    int32_t fraction = 0;
    int32_t scale = 1000000;
    bool negative = false;

    if ((text == (char const *)0) || (out == (int32_t *)0)) {
        return false;
    }
    if (*text == '-') {
        negative = true;
        ++text;
    }
    if ((*text < '0') || (*text > '9')) {
        return false;
    }
    while ((*text >= '0') && (*text <= '9')) {
        whole = (whole * 10) + (int32_t)(*text - '0');
        ++text;
    }
    if (*text == '.') {
        ++text;
        while ((*text >= '0') && (*text <= '9')) {
            if (scale > 1) {
                scale /= 10;
                fraction += (int32_t)(*text - '0') * scale;
            }
            ++text;
        }
    }
    if (*text != '\0') {
        return false;
    }
    *out = negative ? -(whole * 1000000 + fraction) : (whole * 1000000 + fraction);
    return true;
}

/** @brief 检查文本能否安全放进 AT 的引号参数。 */
bool ModemAt_IsSafeText(char const *text) {
    if (text == (char const *)0) {
        return false;
    }
    while (*text != '\0') {
        if ((*text == '"') || (*text == '\r') || (*text == '\n')) {
            return false;
        }
        ++text;
    }
    return true;
}

/** @brief 把文本编码成十六进制字符串。 */
bool ModemAt_HexEncode(char *out, size_t capacity, char const *text) {
    static char const hex[] = "0123456789ABCDEF";
    uint8_t const *bytes = (uint8_t const *)text;
    size_t used = 0U;

    if ((out == (char *)0) || (capacity < 3U) || (text == (char const *)0)) {
        return false;
    }
    while (*bytes != 0U) {
        if ((used + 3U) > capacity) {   /* 两个字符 + 终止符 */
            out[0] = '\0';
            return false;
        }
        out[used++] = hex[((*bytes) >> 4) & 0x0FU];
        out[used++] = hex[(*bytes) & 0x0FU];
        ++bytes;
    }
    out[used] = '\0';
    return (used > 0U);
}

/** @brief 期限是否已到（带符号比较，兼容 32 位回绕）。 */
bool ModemAt_DeadlineReached(uint32_t now_ms, uint32_t deadline_ms) {
    return ((int32_t)(now_ms - deadline_ms) >= 0);
}
