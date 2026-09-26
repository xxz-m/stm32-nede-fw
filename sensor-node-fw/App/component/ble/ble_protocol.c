/*============================================================================
* ble_protocol.c —— BLE 小程序协议实现
* 层次定位：Component 层；保持帧、CRC8、认证与参数业务独立于蓝牙芯片。
*===========================================================================*/
#include "ble_component.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLE_FRAME_PAYLOAD_MAX (17U)
#define BLE_FRAGMENT_DATA_MAX BLE_COMPONENT_FRAGMENT_DATA_MAX

static uint8_t crc8(uint8_t const *data, size_t len) {
    uint8_t crc = 0U;
    uint8_t bit;
    while (len-- > 0U) {
        crc ^= *data++;
        for (bit = 0U; bit < 8U; ++bit) {
            crc = ((crc & 0x80U) != 0U) ? (uint8_t)((crc << 1) ^ 0x07U)
                                          : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static BleParam *findParam(BleComponent *me, char const *name) {
    size_t i;
    for (i = 0U; i < me->param_count; ++i) {
        if (strcmp(me->params[i].name, name) == 0) return &me->params[i];
    }
    return (BleParam *)0;
}

static bool jsonString(char const *json, char const *key, char *out, size_t cap) {
    char pattern[40];
    char const *p;
    size_t used = 0U;
    (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (p == (char const *)0) return false;
    p += strlen(pattern);
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != ':') return false;
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != '"') return false;
    while ((*p != '\0') && (*p != '"')) {
        if ((used + 1U) >= cap) return false;
        out[used++] = *p++;
    }
    if (*p != '"') return false;
    out[used] = '\0';
    return true;
}

static bool jsonRaw(char const *json, char const *key, char *out, size_t cap) {
    char pattern[40];
    char const *p;
    char const *begin;
    size_t len;
    (void)snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    p = strstr(json, pattern);
    if (p == (char const *)0) return false;
    p += strlen(pattern);
    while (isspace((unsigned char)*p)) ++p;
    if (*p++ != ':') return false;
    while (isspace((unsigned char)*p)) ++p;
    begin = p;
    while ((*p != '\0') && (*p != ',') && (*p != '}')) ++p;
    len = (size_t)(p - begin);
    while ((len > 0U) && isspace((unsigned char)begin[len - 1U])) --len;
    if ((len == 0U) || (len >= cap)) return false;
    (void)memcpy(out, begin, len);
    out[len] = '\0';
    return true;
}

static bool enqueue(BleComponent *me, char const *json) {
    size_t len = strlen(json);
    if ((len == 0U) || (len > BLE_COMPONENT_JSON_MAX) ||
        (me->tx_count >= BLE_COMPONENT_TX_DEPTH)) {
        /* NOTE (为什么要报)：发送队列满或单包超长时原来完全静默，手机侧表现为
         *   「没收到应答」而设备日志一片安静，最难定位。这里补一条。 */
        BLE_TRACE(me, "tx dropped");
        return false;
    }
    /* NOTE (手机连上后的详细日志)：这一阶段的流量才需要逐条可见，而配置阶段
     *   （见 ble_component.c 的 traceConfig）只判成败。只打 cmd 与长度，不打 JSON 原文。 */
    {
        char cmd[16];
        char trace[48];
        if (jsonString(json, "cmd", cmd, sizeof(cmd))) {
            (void)snprintf(trace, sizeof(trace), "tx cmd=\"%s\" len=%u",
                           cmd, (unsigned)len);
        } else {
            (void)snprintf(trace, sizeof(trace), "tx len=%u", (unsigned)len);
        }
        BLE_TRACE(me, trace);
    }
    (void)memcpy(me->tx_queue[me->tx_head], json, len + 1U);
    me->tx_head = (uint8_t)((me->tx_head + 1U) % BLE_COMPONENT_TX_DEPTH);
    ++me->tx_count;
    return true;
}

static bool append(char *out, size_t cap, size_t *used, char const *text) {
    size_t len = strlen(text);
    if ((*used + len) >= cap) return false;
    (void)memcpy(&out[*used], text, len + 1U);
    *used += len;
    return true;
}

static bool formatValue(BleParam const *param, char *out, size_t cap) {
    int n = -1;
    if (param->value.type == BLE_VALUE_BOOL) {
        n = snprintf(out, cap, "%s", param->value.value.boolean ? "true" : "false");
    } else if (param->value.type == BLE_VALUE_INT) {
        n = snprintf(out, cap, "%ld", (long)param->value.value.integer);
    } else if (param->value.type == BLE_VALUE_STRING) {
        n = snprintf(out, cap, "\"%s\"", param->value.value.string);
    } else if (param->value.type == BLE_VALUE_DECIMAL) {
        int32_t value = param->value.value.decimal.mantissa;
        uint8_t decimals = param->value.value.decimal.decimals;
        uint32_t scale = 1U;
        uint8_t i;
        for (i = 0U; i < decimals; ++i) scale *= 10U;
        if (decimals == 0U) n = snprintf(out, cap, "%ld", (long)value);
        else if (decimals == 1U) n = snprintf(out, cap, "%s%lu.%01lu", value < 0 ? "-" : "",
                          (unsigned long)((uint32_t)(value < 0 ? -value : value) / scale),
                          (unsigned long)((uint32_t)(value < 0 ? -value : value) % scale));
        else if (decimals == 2U) n = snprintf(out, cap, "%s%lu.%02lu", value < 0 ? "-" : "",
                          (unsigned long)((uint32_t)(value < 0 ? -value : value) / scale),
                          (unsigned long)((uint32_t)(value < 0 ? -value : value) % scale));
        else n = snprintf(out, cap, "%s%lu.%03lu", value < 0 ? "-" : "",
                          (unsigned long)((uint32_t)(value < 0 ? -value : value) / scale),
                          (unsigned long)((uint32_t)(value < 0 ? -value : value) % scale));
    }
    return (n >= 0) && ((size_t)n < cap);
}

static void resetSession(BleComponent *me) {
    me->authenticated = false;
    me->reboot_pending = false;
    me->rx_frame_len = 0U;
    me->rx_json_len = 0U;
    me->rx_total = 0U;
    me->rx_next_sequence = 0U;
    me->tx_head = 0U;
    me->tx_tail = 0U;
    me->tx_count = 0U;
    me->last_json[0] = '\0';
}

static void buildParams(BleComponent *me) {
    char part[160];
    size_t used = 0U;
    size_t i;
    me->work_json[0] = '\0';
    if (!append(me->work_json, sizeof(me->work_json), &used, "{\"cmd\":\"params\",\"params\":[")) return;
    for (i = 0U; i < me->param_count; ++i) {
        BleParam const *p = &me->params[i];
        char const *type = (p->value.type == BLE_VALUE_BOOL) ? "bool" :
                           (p->value.type == BLE_VALUE_INT) ? "int" :
                           (p->value.type == BLE_VALUE_STRING) ? "string" : "float";
        (void)snprintf(part, sizeof(part), "%s{\"param\":\"%s\",\"type\":\"%s\",\"writable\":%s,\"label\":\"%s\"%s%s%s}",
                       i == 0U ? "" : ",", p->name, type, p->writable ? "true" : "false",
                       p->label, p->unit != (char const *)0 ? ",\"unit\":\"" : "",
                       p->unit != (char const *)0 ? p->unit : "",
                       p->unit != (char const *)0 ? "\"" : "");
        if (!append(me->work_json, sizeof(me->work_json), &used, part)) return;
    }
    if (append(me->work_json, sizeof(me->work_json), &used, "]}")) (void)enqueue(me, me->work_json);
}

static void handleJson(BleComponent *me, char *json) {
    char cmd[16];
    char name[64];
    char raw[64];
    char reply[256];
    char trace[40];
    BleParam *param;
    if (!jsonString(json, "cmd", cmd, sizeof(cmd))) {
        BLE_TRACE(me, "rx invalid request");
        (void)enqueue(me, "{\"cmd\":\"auth\",\"ok\":false,\"err\":1002,\"msg\":\"invalid auth request\"}");
        return;
    }
    /* NOTE (只打 cmd，不打 JSON 原文)：auth 请求里带设备 PIN，运行期日志不得出现凭据
     *   （见 AGENTS.md 5.2 与凭据纪律）。要哪一项由 param 字段体现，设备的应答里也有。 */
    (void)snprintf(trace, sizeof(trace), "rx cmd=\"%s\"", cmd);
    BLE_TRACE(me, trace);
    if (strcmp(cmd, "auth") == 0) {
        char pin[32];
        if (!jsonString(json, "pin", pin, sizeof(pin))) {
            (void)enqueue(me, "{\"cmd\":\"auth\",\"ok\":false,\"err\":1002,\"msg\":\"invalid auth request\"}");
        } else if (strcmp(pin, me->config->pin) != 0) {
            me->authenticated = false;
            BLE_TRACE(me, "authentication rejected");
            (void)enqueue(me, "{\"cmd\":\"auth\",\"ok\":false,\"err\":1001,\"msg\":\"invalid pin\"}");
        } else {
            me->authenticated = true;
            BLE_TRACE(me, "authentication accepted");
            (void)snprintf(reply, sizeof(reply), "{\"cmd\":\"auth\",\"ok\":true,\"sn\":\"%s\"}", me->config->serial_number);
            (void)enqueue(me, reply);
            (void)snprintf(reply, sizeof(reply),
                           "{\"cmd\":\"device_info\",\"deviceName\":\"%s\",\"macAddress\":\"%s\",\"modelCode\":\"%s\",\"batchCode\":\"%s\"}",
                           me->config->device_name, me->mac_address,
                           me->config->model_code, me->config->batch_code);
            (void)enqueue(me, reply);
        }
        return;
    }
    if (!me->authenticated) {
        (void)enqueue(me, "{\"cmd\":\"error\",\"ok\":false,\"err\":1003,\"msg\":\"not authenticated\"}");
        return;
    }
    if (strcmp(cmd, "ping") == 0) (void)enqueue(me, "{\"cmd\":\"pong\"}");
    else if (strcmp(cmd, "params") == 0) buildParams(me);
    else if (strcmp(cmd, "nak") == 0) {
        if (me->last_json[0] != '\0') (void)enqueue(me, me->last_json);
    } else if (strcmp(cmd, "get") == 0) {
        if (!jsonString(json, "param", name, sizeof(name))) name[0] = '\0';
        param = findParam(me, name);
        if (param == (BleParam *)0) (void)snprintf(reply, sizeof(reply), "{\"cmd\":\"get\",\"param\":\"%s\",\"ok\":false,\"err\":1}", name);
        else {
            (void)formatValue(param, raw, sizeof(raw));
            (void)snprintf(reply, sizeof(reply), "{\"cmd\":\"get\",\"param\":\"%s\",\"ok\":true,\"v\":%s}", name, raw);
        }
        (void)enqueue(me, reply);
    } else if (strcmp(cmd, "set") == 0) {
        BleValue next;
        int err = 0;
        if (!jsonString(json, "param", name, sizeof(name)) || !jsonRaw(json, "v", raw, sizeof(raw))) err = 2;
        param = err == 0 ? findParam(me, name) : (BleParam *)0;
        if ((err == 0) && (param == (BleParam *)0)) err = 1;
        else if ((err == 0) && !param->writable) err = 3;
        if (err == 0) {
            next = param->value;
            if (next.type == BLE_VALUE_BOOL) {
                if (strcmp(raw, "true") == 0) next.value.boolean = true;
                else if (strcmp(raw, "false") == 0) next.value.boolean = false;
                else err = 2;
            } else if (next.type == BLE_VALUE_INT) {
                long value = strtol(raw, (char **)0, 10);
                if ((value < param->minimum) || (value > param->maximum)) err = 2;
                else next.value.integer = (int32_t)value;
            } else err = 2;
            if ((err == 0) && (param->changed != (BleParamChanged)0) &&
                (param->changed(name, &next, param->context) != 0)) err = 2;
            if (err == 0) param->value = next;
        }
        (void)snprintf(reply, sizeof(reply), "{\"cmd\":\"set\",\"param\":\"%s\",\"ok\":%s,\"err\":%d}", name, err == 0 ? "true" : "false", err);
        (void)enqueue(me, reply);
    } else if (strcmp(cmd, "reboot") == 0) {
        me->reboot_pending = true;
        (void)enqueue(me, "{\"cmd\":\"reboot\",\"ok\":true}");
    } else {
        (void)enqueue(me, "{\"cmd\":\"error\",\"ok\":false,\"err\":1001,\"msg\":\"unknown command\"}");
    }
}

void BLE_Protocol_Line(BleComponent *me, char const *line) {
    extern bool BLE_Backend_LineConnected(char const *);
    extern bool BLE_Backend_LineDisconnected(char const *);
    if (BLE_Backend_LineConnected(line)) {
        me->connected = true;
        resetSession(me);
        BLE_TRACE(me, "link connected");
    }
    else if (BLE_Backend_LineDisconnected(line)) {
        me->connected = false;
        resetSession(me);
        BLE_TRACE(me, "link disconnected");
    }
}

void BLE_Protocol_Payload(BleComponent *me, uint8_t const *payload, uint16_t length) {
    uint16_t total;
    uint8_t seq;
    size_t chunk;
    if ((length > 0U) && (payload[0] == (uint8_t)'{')) {
        if (length <= BLE_COMPONENT_JSON_MAX) {
            (void)memcpy(me->rx_json, payload, length);
            me->rx_json[length] = '\0';
            handleJson(me, me->rx_json);
        } else {
            BLE_TRACE(me, "rx json too long");
        }
        return;
    }
    if (length < 4U) { me->rx_total = 0U; return; }
    seq = payload[0];
    total = (uint16_t)(((uint16_t)payload[1] << 8) | payload[2]);
    chunk = (size_t)length - 3U;
    if (seq == 0U) { me->rx_total = total; me->rx_json_len = 0U; me->rx_next_sequence = 0U; }
    if ((total > BLE_COMPONENT_JSON_MAX) || (total != me->rx_total) ||
        (seq != me->rx_next_sequence) || ((me->rx_json_len + chunk) > total)) {
        /* NOTE (为什么要报)：分片乱序、总长不符、超长都会整包丢弃，原来完全静默——
         *   手机侧表现为「发了没反应」，而对端又看不到任何异常。这条是唯一的线索。 */
        BLE_TRACE(me, "rx frag dropped");
        me->rx_total = 0U;
        return;
    }
    (void)memcpy(&me->rx_json[me->rx_json_len], &payload[3], chunk);
    me->rx_json_len += chunk;
    ++me->rx_next_sequence;
    if (me->rx_json_len == me->rx_total) {
        me->rx_json[me->rx_json_len] = '\0';
        handleJson(me, me->rx_json);
        me->rx_total = 0U;
    }
}

bool BLE_Protocol_SendJson(BleComponent *me, char const *json) {
    uint8_t frame[BLE_COMPONENT_FRAME_MAX];
    uint8_t payload[BLE_FRAME_PAYLOAD_MAX];
    size_t length = strlen(json);
    size_t offset = 0U;
    uint8_t seq = 0U;
    if (!me->connected || (length == 0U) || (length > BLE_COMPONENT_JSON_MAX)) return false;
    while (offset < length) {
        size_t chunk;
        size_t payload_len;
        if (length <= BLE_FRAME_PAYLOAD_MAX) {
            chunk = length;
            (void)memcpy(payload, json, chunk);
            payload_len = chunk;
            offset = length;
        } else {
            chunk = length - offset;
            if (chunk > BLE_FRAGMENT_DATA_MAX) chunk = BLE_FRAGMENT_DATA_MAX;
            payload[0] = seq++;
            payload[1] = (uint8_t)(length >> 8);
            payload[2] = (uint8_t)length;
            (void)memcpy(&payload[3], &json[offset], chunk);
            payload_len = chunk + 3U;
            offset += chunk;
        }
        frame[0] = (uint8_t)(payload_len >> 8);
        frame[1] = (uint8_t)payload_len;
        (void)memcpy(&frame[2], payload, payload_len);
        frame[2U + payload_len] = crc8(payload, payload_len);
        if (me->port->write(me->port->context, frame, payload_len + 3U) != BLE_PORT_OK) return false;
    }
    return true;
}

void BLE_Protocol_Consume(BleComponent *me, uint8_t byte) {
    uint16_t len;
    size_t frame_len;
    if ((me->rx_frame_len == 0U) && (me->line_len == 0U) &&
        ((byte == (uint8_t)'\r') || (byte == (uint8_t)'\n'))) {
        return;
    }
    if ((me->line_len > 0U) || (byte == (uint8_t)'+')) {
        if (me->line_len < (sizeof(me->line) - 1U)) me->line[me->line_len++] = (char)byte;
        if ((byte == (uint8_t)'\r') || (byte == (uint8_t)'\n') || (byte == 0U)) {
            me->line[me->line_len] = '\0';
            BLE_Protocol_Line(me, me->line);
            me->line_len = 0U;
        }
        return;
    }
    if (me->rx_frame_len < sizeof(me->rx_frame)) me->rx_frame[me->rx_frame_len++] = byte;
    else me->rx_frame_len = 0U;
    while (me->rx_frame_len >= 3U) {
        len = (uint16_t)(((uint16_t)me->rx_frame[0] << 8) | me->rx_frame[1]);
        if ((len == 0U) || (len > BLE_FRAME_PAYLOAD_MAX)) {
            (void)memmove(me->rx_frame, &me->rx_frame[1], --me->rx_frame_len);
            continue;
        }
        frame_len = (size_t)len + 3U;
        if (me->rx_frame_len < frame_len) return;
        if (crc8(&me->rx_frame[2], len) == me->rx_frame[2U + len]) {
            BLE_TRACE(me, "RX frame CRC OK");
            BLE_Protocol_Payload(me, &me->rx_frame[2], len);
            (void)memmove(me->rx_frame, &me->rx_frame[frame_len], me->rx_frame_len - frame_len);
            me->rx_frame_len -= frame_len;
        } else {
            BLE_TRACE(me, "RX frame CRC error");
            (void)memmove(me->rx_frame, &me->rx_frame[1], --me->rx_frame_len);
        }
    }
}

bool BLE_Protocol_Enqueue(BleComponent *me, char const *json) { return enqueue(me, json); }
