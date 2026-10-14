/* Component层：云端命令JSON字符串编解码；标准C，无HAL/QP/动态内存。 */
#include "modem_command.h"
#include <string.h>

/** @brief JSON对象外允许标准空白，但字符串内协议只允许可打印ASCII。 */
static void skipSpace(char const **p) {
    while (**p == ' ' || **p == '\r' || **p == '\n' || **p == '\t') { ++*p; }
}

/** @brief 解析一个JSON字符串，支持ASCII的Unicode转义，失败不保留截断字段。 */
static bool readString(char const **cursor, char *out, size_t capacity) {
    char const *p = *cursor;
    size_t used = 0U;
    if (capacity == 0U) { return false; }
    out[0] = '\0';
    if (*p++ != '"') { return false; }
    while (*p != '\0' && *p != '"') {
        unsigned c = (unsigned char)*p++;
        if (c == '\\') {
            c = (unsigned char)*p++;
            if (c == 'u') {
                unsigned i, value = 0U;
                for (i = 0U; i < 4U; ++i) {
                    unsigned d = (unsigned char)*p;
                    if (d >= '0' && d <= '9') { d -= '0'; }
                    else if (d >= 'A' && d <= 'F') { d = d - 'A' + 10U; }
                    else if (d >= 'a' && d <= 'f') { d = d - 'a' + 10U; }
                    else { out[0] = '\0'; return false; }
                    value = (value << 4U) | d; ++p;
                }
                c = value;
            } else if (c != '"' && c != '\\' && c != '/') { out[0] = '\0'; return false; }
        }
        if (c < 0x20U || c > 0x7EU || used + 1U >= capacity) { out[0] = '\0'; return false; }
        out[used++] = (char)c;
    }
    if (*p != '"') { out[0] = '\0'; return false; }
    out[used] = '\0'; *cursor = p + 1; return true;
}

bool Modem_Command_Parse(char const *json, struct ModemCommandRequest *request) {
    char const *p = json;
    unsigned fields = 0U;
    if (request == NULL) { return false; }
    memset(request, 0, sizeof(*request));
    if (p == NULL) { return false; }
    skipSpace(&p);
    if (*p++ != '{') { return false; }
    for (;;) {
        char key[8];
        char *out;
        size_t capacity;
        unsigned flag;
        skipSpace(&p);
        if (!readString(&p, key, sizeof(key))) { return false; }
        if (strcmp(key, "id") == 0) { flag = 1U; out = request->id; capacity = sizeof(request->id); }
        else if (strcmp(key, "command") == 0) { flag = 2U; out = request->command; capacity = sizeof(request->command); }
        else if (strcmp(key, "data") == 0) { flag = 4U; out = request->data; capacity = sizeof(request->data); }
        else { return false; }
        if ((fields & flag) != 0U) { return false; }
        skipSpace(&p); if (*p++ != ':') { return false; } skipSpace(&p);
        if (!readString(&p, out, capacity)) { return false; }
        fields |= flag;
        skipSpace(&p);
        if (*p == '}') { ++p; break; }
        if (*p++ != ',') { return false; }
    }
    skipSpace(&p);
    request->has_data = (fields & 4U) != 0U;
    return *p == '\0' && (fields & 3U) == 3U && request->id[0] != '\0' && request->command[0] != '\0';
}

/** @brief 逐字节有界输出，不能按最坏转义长度误拒绝实际放得下的回复。 */
static bool putChar(char *out, size_t capacity, size_t *used, char c) {
    if (*used + 1U >= capacity) { return false; }
    out[(*used)++] = c; out[*used] = '\0'; return true;
}
/** @brief 追加固定JSON片段，容量检查与转义输出使用同一个边界。 */
static bool putText(char *out, size_t capacity, size_t *used, char const *text) {
    while (*text != '\0') { if (!putChar(out, capacity, used, *text++)) { return false; } }
    return true;
}
/** @brief 对字符串完整转义；基础AT的CR/LF只出现在JSON字符串内。 */
static bool putString(char *out, size_t capacity, size_t *used, char const *text) {
    static char const hex[] = "0123456789ABCDEF";
    if (!putChar(out, capacity, used, '"')) { return false; }
    while (*text != '\0') {
        unsigned char c = (unsigned char)*text++;
        if (c == '"' || c == '\\') {
            if (!putChar(out, capacity, used, '\\') || !putChar(out, capacity, used, (char)c)) { return false; }
        } else if (c == '\r' || c == '\n' || c == '\t') {
            if (!putChar(out, capacity, used, '\\') ||
                !putChar(out, capacity, used, c == '\r' ? 'r' : c == '\n' ? 'n' : 't')) { return false; }
        } else if (c < 0x20U) {
            if (!putText(out, capacity, used, "\\u00") || !putChar(out, capacity, used, hex[c >> 4U]) ||
                !putChar(out, capacity, used, hex[c & 15U])) { return false; }
        } else if (!putChar(out, capacity, used, (char)c)) { return false; }
    }
    return putChar(out, capacity, used, '"');
}

bool Modem_Command_Format(char *out, size_t capacity, char const *id,
                           char const *command, char const *data) {
    size_t used = 0U;
    bool basic, ok;
    if (out == NULL || capacity == 0U || id == NULL || data == NULL) { return false; }
    out[0] = '\0';
    basic = command != NULL && (strcmp(command, "AT") == 0 || strcmp(command, "AT+RST") == 0 ||
                                 strcmp(command, "AT+ZERO=\"CONFIRM\"") == 0);
    /* 帧级ERROR同样遵守寄存器纯负载约定，只有基础命令保留结果行。 */
    if (!basic && strcmp(data, "ERROR\r\n") == 0) { data = "ERROR"; }
    ok = putText(out, capacity, &used, "{\"id\":") && putString(out, capacity, &used, id);
    if (ok && basic) { ok = putText(out, capacity, &used, ",\"command\":") && putString(out, capacity, &used, command); }
    ok = ok && putText(out, capacity, &used, ",\"data\":") && putString(out, capacity, &used, data) &&
         putChar(out, capacity, &used, '}');
    if (!ok) { out[0] = '\0'; }
    return ok;
}
