/*============================================================================
* ble_vg6328.c —— VG6328 编译期后端
* 层次定位：Component/backend；只生成 AT 文本，不接触 UART/HAL。
*===========================================================================*/
#include "ble_backend.h"
#include <stdio.h>
#include <string.h>

#if (BLE_COMPONENT_BACKEND == BLE_BACKEND_VG6328)

static bool compactUuid(char const *source, char out[33]) {
    size_t used = 0U;
    while (*source != '\0') {
        if (*source != '-') {
            if (used >= 32U) return false;
            out[used++] = *source;
        }
        ++source;
    }
    out[used] = '\0';
    return used == 32U;
}

uint8_t BLE_Backend_CommandCount(void) { return 16U; }

char const *BLE_Backend_Command(BleConfig const *cfg, uint8_t step,
                                char *out, size_t cap) {
    int n;
    switch (step) {
        case 0U: return "AT\r\n\0";
        case 1U: return "AT+BTM?\r\n\0";
        case 2U: return "AT+BTM=0\r\n\0";
        case 3U: return "AT+ADE?\r\n\0";
        case 4U: return "AT+ADE=1\r\n\0";
        case 5U: return "AT+NAM?\r\n\0";
        case 6U:
            n = snprintf(out, cap, "AT+NAM=%s\r\n%c", cfg->local_name, '\0');
            return ((n > 0) && ((size_t)n < cap)) ? out : (char const *)0;
        case 7U: return "AT+ULEN?\r\n\0";
        case 8U: return "AT+ULEN=128\r\n\0";
        case 9U: return "AT+UUIDX?\r\n\0";
        case 10U: {
            char service[33];
            char rx[33];
            char tx[33];
            if (!compactUuid(cfg->service_uuid, service)) return (char const *)0;
            (void)memcpy(rx, service, sizeof(rx));
            (void)memcpy(tx, service, sizeof(tx));
            rx[7] = '1';
            tx[7] = '2';
            n = snprintf(out, cap, "AT+UUIDX=%s,%s,%s,%s,0000\r\n%c",
                         service, rx, service, tx, '\0');
            return ((n > 0) && ((size_t)n < cap)) ? out : (char const *)0;
        }
        case 11U: return "AT+ADD?\r\n\0";
        case 12U: {
            char service[33];
            char reversed[33];
            size_t i;
            if (!compactUuid(cfg->service_uuid, service)) return (char const *)0;
            for (i = 0U; i < 16U; ++i) {
                reversed[i * 2U] = service[(15U - i) * 2U];
                reversed[i * 2U + 1U] = service[(15U - i) * 2U + 1U];
            }
            reversed[32] = '\0';
            n = snprintf(out, cap, "AT+ADD=HEX1107%s\r\n%c", reversed, '\0');
            return ((n > 0) && ((size_t)n < cap)) ? out : (char const *)0;
        }
        case 13U: return "AT+RST=1\r\n\0";
        case 14U: return "AT\r\n\0";
        case 15U: return "AT+MAC?\r\n\0";
        default: return (char const *)0;
    }
}

bool BLE_Backend_LineConnected(char const *line) {
    return (strstr(line, "+CONNECT-") != (char const *)0);
}

bool BLE_Backend_LineDisconnected(char const *line) {
    return (strstr(line, "+DISCONNECT-") != (char const *)0);
}

void BLE_Backend_ParseLine(BleComponent *me, char const *line) {
    char const *mac = strstr(line, "+MAC:");
    size_t i = 0U;
    if (mac == (char const *)0) return;
    mac += 5;
    while ((i < 17U) && (mac[i] != '\0') && (mac[i] != '\r') && (mac[i] != '\n')) {
        me->mac_address[i] = mac[i];
        ++i;
    }
    me->mac_address[i] = '\0';
}

bool BLE_Backend_ResponseMatches(BleConfig const *cfg, uint8_t step,
                                 char const *line) {
    char expected[176];
    char service[33];
    char reversed[33];
    size_t i;
    if (step == 1U) return strstr(line, "+BTM:0") != (char const *)0;
    if (step == 3U) return strstr(line, "+ADE:1") != (char const *)0;
    if (step == 5U) {
        (void)snprintf(expected, sizeof(expected), "+NAM:%s", cfg->local_name);
        return strstr(line, expected) != (char const *)0;
    }
    if (step == 7U) return strstr(line, "+ULEN:128") != (char const *)0;
    if (!compactUuid(cfg->service_uuid, service)) return false;
    if (step == 9U) {
        char rx[33];
        char tx[33];
        (void)memcpy(rx, service, sizeof(rx));
        (void)memcpy(tx, service, sizeof(tx));
        rx[7] = '1'; tx[7] = '2';
        (void)snprintf(expected, sizeof(expected), "+UUIDX:%s,%s,%s,%s", service, rx, service, tx);
        return strstr(line, expected) != (char const *)0;
    }
    if (step == 11U) {
        for (i = 0U; i < 16U; ++i) {
            reversed[i * 2U] = service[(15U - i) * 2U];
            reversed[i * 2U + 1U] = service[(15U - i) * 2U + 1U];
        }
        reversed[32] = '\0';
        (void)snprintf(expected, sizeof(expected), "+ADD:HEX1107%s", reversed);
        return strstr(line, expected) != (char const *)0;
    }
    if (step == 15U) return strstr(line, "+MAC:") != (char const *)0;
    return false;
}

bool BLE_Backend_ResponseComplete(uint8_t step, char const *line) {
    if (step == 1U) return strstr(line, "+BTM:") != (char const *)0;
    if (step == 3U) return strstr(line, "+ADE:") != (char const *)0;
    if (step == 5U) return strstr(line, "+NAM:") != (char const *)0;
    if (step == 7U) return strstr(line, "+ULEN:") != (char const *)0;
    if (step == 9U) return strstr(line, "+UUIDX:") != (char const *)0;
    if (step == 11U) return strstr(line, "+ADD:") != (char const *)0;
    if (step == 15U) return strstr(line, "+MAC:") != (char const *)0;
    return false;
}

#else
#error "Unsupported BLE component backend"
#endif
