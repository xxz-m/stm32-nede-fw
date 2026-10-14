/* 服务层：寄存器ASCII负载、Modbus CRC和固定状态标记；标准C，无硬件/QP依赖。 */
#include "svc_register.h"
#include "svc_frame.h"
#include <stdio.h>
#include <string.h>

/** @brief 只接受协议规定的大写十六进制，拒绝空白和符号。 */
static bool hexValue(char const *text, unsigned width, uint16_t *value) {
    unsigned i;
    uint16_t v = 0U;
    for (i = 0U; i < width; ++i) {
        unsigned char c = (unsigned char)text[i];
        unsigned digit;
        if (c >= '0' && c <= '9') { digit = c - '0'; }
        else if (c >= 'A' && c <= 'F') { digit = c - 'A' + 10U; }
        else { return false; }
        v = (uint16_t)((v << 4U) | digit);
    }
    *value = v;
    return true;
}

/** @brief 帧级错误不含CRC，也不输出部分逐项结果。 */
static void frameError(SvcRegisterResult *out, unsigned code) {
    out->frame_error = true;
    out->frame_code = code;
    out->all_ok = false;
    (void)snprintf(out->data, sizeof(out->data), "+CME ERROR:%u", code);
}

/** @brief 状态码是固定ASCII文本，不与正常四位数据共用数值空间。 */
static char const *statusText(SvcRegisterStatus status) {
    static char const *const codes[] = {"00", "XY", "XX", "RO", "OV", "IC", "NP", "SE", "NR"};
    return (unsigned)status < sizeof(codes) / sizeof(codes[0]) ? codes[status] : "NR";
}

void SVC_Register_Execute(bool write, char const *data,
                          SvcRegisterPort const *port, SvcRegisterResult *out) {
    char copy[APP_CFG_REGISTER_REQUEST_SIZE];
    char *fields[APP_CFG_REGISTER_READ_MAX + 1U];
    uint16_t addresses[APP_CFG_REGISTER_READ_MAX];
    char const *last;
    size_t length, count = 0U, pairs, i, used = 0U;
    uint16_t wire, crc;
    char *cursor;
    if (out == NULL) { return; }
    memset(out, 0, sizeof(*out));
    if (data == NULL || port == NULL || port->read == NULL || port->write == NULL) {
        frameError(out, 2U); return;
    }
    /* 有界检查后才复制，不能让超长请求截断成另一条合法指令。 */
    for (length = 0U; length < sizeof(copy) && data[length] != '\0'; ++length) { }
    if (length == 0U || length >= sizeof(copy)) { frameError(out, 2U); return; }
    last = strrchr(data, ',');
    if (last == NULL) { frameError(out, 2U); return; }
    memcpy(copy, data, length + 1U);
    cursor = copy;
    for (i = 0U; i <= length; ++i) {
        if (copy[i] == ',' || copy[i] == '\0') {
            if (count >= sizeof(fields) / sizeof(fields[0])) { frameError(out, 2U); return; }
            fields[count++] = cursor;
            copy[i] = '\0'; cursor = &copy[i + 1U];
        }
    }
    if (count < 2U || (write && ((count - 1U) % 2U != 0U))) { frameError(out, 2U); return; }
    pairs = write ? (count - 1U) / 2U : count - 1U;
    if (pairs == 0U || pairs > (write ? APP_CFG_REGISTER_WRITE_MAX : APP_CFG_REGISTER_READ_MAX)) {
        frameError(out, 2U); return;
    }
    if (strlen(fields[count - 1U]) != 4U || !hexValue(fields[count - 1U], 4U, &wire)) {
        frameError(out, 2U); return;
    }
    for (i = 0U; i < pairs; ++i) {
        size_t index = write ? i * 2U : i;
        if (strlen(fields[index]) != 2U || !hexValue(fields[index], 2U, &addresses[i]) ||
            (write && strlen(fields[index + 1U]) != 4U)) {
            frameError(out, 2U); return;
        }
    }
    crc = SVC_Frame_Crc16((uint8_t const *)data, (size_t)(last - data));
    if (wire != (uint16_t)((crc << 8U) | (crc >> 8U))) { frameError(out, 6U); return; }
    out->all_ok = true;
    for (i = 0U; i < pairs; ++i) {
        uint16_t value = 0U;
        uint8_t address = (uint8_t)addresses[i];
        SvcRegisterStatus status;
        int n;
        if (write) {
            status = hexValue(fields[i * 2U + 1U], 4U, &value)
                   ? port->write(port->context, address, value) : SVC_REG_IC;
        } else { status = port->read(port->context, address, &value); }
        if (status != SVC_REG_OK) { out->all_ok = false; }
        if (!write && status == SVC_REG_OK) {
            n = snprintf(out->data + used, sizeof(out->data) - used,
                         "%s%02X,%04X", i == 0U ? "" : ",", (unsigned)address, (unsigned)value);
        } else {
            char const *code = write ? statusText(status) : (status == SVC_REG_XY ? "XY" : "XX");
            n = snprintf(out->data + used, sizeof(out->data) - used,
                         "%s%02X,%s", i == 0U ? "" : ",", (unsigned)address, code);
        }
        if (n < 0 || (size_t)n >= sizeof(out->data) - used) { frameError(out, 2U); return; }
        used += (size_t)n;
    }
    crc = SVC_Frame_Crc16((uint8_t const *)out->data, used);
    (void)snprintf(out->data + used, sizeof(out->data) - used,
                   ",%02X%02X", (unsigned)(crc & 0xFFU), (unsigned)(crc >> 8U));
}
