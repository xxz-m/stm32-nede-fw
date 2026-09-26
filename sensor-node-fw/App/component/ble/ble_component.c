/*============================================================================
* ble_component.c —— 可替换后端的 BLE 组件状态机
* 层次定位：Component 层；所有推进均由 Process 驱动，不阻塞等待。
*===========================================================================*/
#include "ble_component.h"
#include "backend/ble_backend.h"
#include <stdio.h>
#include <string.h>

void BLE_Protocol_Consume(BleComponent *, uint8_t);
bool BLE_Protocol_SendJson(BleComponent *, char const *);

static bool elapsed(uint32_t now, uint32_t deadline) {
    return ((int32_t)(now - deadline) >= 0);
}

/*--------------------------------------------------------------- 配置阶段诊断 */
#define BLE_DIAG_FIELD_MAX (40U)
#define BLE_DIAG_LINE_MAX  (120U)

#if (BLE_COMPONENT_TRACE_ENABLE != 0U)
/** @brief 把源文本压成一行可打印 ASCII（取第一行、非可打印字符换成 '.'、限长）。
*  NOTE (为什么清洗)：AT 命令尾部带 CRLF 与结尾 NUL，模组应答也可能混入控制字符；
*   而运行期输出必须保持 ASCII 单行（见 AGENTS.md 5.2），原样进日志会把一行拆成几行。 */
static void sanitize(char const *source, char *out, size_t capacity) {
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

/** @brief 配置步骤的诊断：**成功只给「第几步 + 正在写哪一项」，失败才带命令与应答**。
*  NOTE (为什么要分开，2026-09-19 按用户要求)：配置表有 16 步，而且查询步匹配上时还会
*   跳过对应的设置步，一次开机就是十来行。开这个阶段手机还没连进来，现场需要的只是
*   「配置成功没有」这一个判断，所以成功压成 `config k/16 cmd="…"`（带上正在写哪一项，
*   排 BLE 问题才知道走到哪）；只有写失败 / 应答 ERROR / 超时才把 rx 与 reason 一起打出来。
*  NOTE (为什么命令能重新生成)：本后端的 Command() 是纯函数、只依赖配置与步号，
*   所以失败点不必额外保存命令文本，按当前步号重建即可。 */
static void traceConfig(BleComponent *me, BleFailure reason) {
    char raw[160];
    char command[BLE_DIAG_FIELD_MAX];
    char reply[BLE_DIAG_FIELD_MAX];
    char line[BLE_DIAG_LINE_MAX];
    char const *text = BLE_Backend_Command(me->config, me->backend_step,
                                           raw, sizeof(raw));
    int produced;

    sanitize(text, command, sizeof(command));
    if (reason == BLE_FAILURE_NONE) {
        produced = snprintf(line, sizeof(line), "config %u/%u cmd=\"%s\"",
                            (unsigned)me->backend_step + 1U,
                            (unsigned)BLE_Backend_CommandCount(), command);
    } else {
        sanitize(me->line, reply, sizeof(reply));
        produced = snprintf(line, sizeof(line),
                            "config %u/%u fail cmd=\"%s\" rx=\"%s\" reason=%u",
                            (unsigned)me->backend_step + 1U,
                            (unsigned)BLE_Backend_CommandCount(), command, reply,
                            (unsigned)reason);
    }
    if (produced > 0) {
        BLE_TRACE(me, line);
    }
}
#else
/* trace 关掉时整块在编译期消失，调用点不用写 if。 */
#define traceConfig(me_, reason_) ((void)0)
#endif

static void sendBackendStep(BleComponent *me) {
    char command[160];
    char const *text = BLE_Backend_Command(me->config, me->backend_step,
                                           command, sizeof(command));
    if ((text == (char const *)0) ||
        (me->port->write(me->port->context, (uint8_t const *)text,
                         strlen(text) + 1U) != BLE_PORT_OK)) {
        me->failure = BLE_FAILURE_PORT_WRITE;
        me->failure_step = me->backend_step;
        me->state = BLE_COMPONENT_FAILED;
        traceConfig(me, BLE_FAILURE_PORT_WRITE);
        return;
    }
    traceConfig(me, BLE_FAILURE_NONE);
    if (me->backend_step == 13U) {
        me->command_pending = false;
        me->restart_waiting = true;
        me->restart_probe_attempts = 0U;
        me->deadline_ms = me->port->get_tick_ms(me->port->context) +
                          BLE_COMPONENT_RESTART_WAIT_MS;
        return;
    }
    me->command_pending = true;
    me->deadline_ms = me->port->get_tick_ms(me->port->context) + BLE_COMPONENT_TIMEOUT_MS;
}

void BLE_Component_Init(BleComponent *me, BlePort const *port,
                        BleConfig const *config, BleParam *params, size_t count) {
    (void)memset(me, 0, sizeof(*me));
    me->port = port;
    me->config = config;
    me->params = params;
    me->param_count = (count > BLE_COMPONENT_MAX_PARAMS) ? BLE_COMPONENT_MAX_PARAMS : count;
    (void)strcpy(me->mac_address, "unknown");
}

void BLE_Component_Start(BleComponent *me) {
    if ((me == (BleComponent *)0) || (me->port == (BlePort const *)0) ||
        (me->config == (BleConfig const *)0)) return;
    me->state = BLE_COMPONENT_PROBING;
    me->backend_step = 0U;
    me->command_pending = false;
    me->line_len = 0U;
    me->rx_frame_len = 0U;
    me->backend_match = false;
    me->configuration_changed = false;
    me->restart_waiting = false;
    me->restart_probe_attempts = 0U;
    me->failure = BLE_FAILURE_NONE;
    me->failure_step = 0U;
    sendBackendStep(me);
}

void BLE_Component_Process(BleComponent *me) {
    uint8_t data[BLE_COMPONENT_RX_BUDGET];
    size_t count;
    size_t i;
    uint32_t now;
    if ((me == (BleComponent *)0) || (me->state == BLE_COMPONENT_STOPPED)) return;
    now = me->port->get_tick_ms(me->port->context);
    if (me->restart_waiting) {
        /* 丢弃模组重启期间的启动文本，等待配置时间后重新用 AT 探测。 */
        (void)me->port->read(me->port->context, data, sizeof(data));
        if (!elapsed(now, me->deadline_ms)) return;
        me->restart_waiting = false;
        me->backend_step = 14U;
        ++me->restart_probe_attempts;
        sendBackendStep(me);
        return;
    }
    count = me->port->read(me->port->context, data, sizeof(data));
    for (i = 0U; i < count; ++i) {
        if (me->command_pending) {
            if (me->line_len < (sizeof(me->line) - 1U)) me->line[me->line_len++] = (char)data[i];
            if ((data[i] == (uint8_t)'\r') || (data[i] == (uint8_t)'\n') ||
                (data[i] == 0U)) {
                me->line[me->line_len] = '\0';
                if (strstr(me->line, "OK") != (char *)0) {
                    me->command_pending = false;
                    me->line_len = 0U;
                    if (((me->backend_step == 1U) || (me->backend_step == 3U) ||
                         (me->backend_step == 5U) || (me->backend_step == 7U) ||
                         (me->backend_step == 9U) || (me->backend_step == 11U)) &&
                        me->backend_match) {
                        me->backend_step = (uint8_t)(me->backend_step + 2U);
                    } else {
                        if ((me->backend_step == 2U) || (me->backend_step == 4U) ||
                            (me->backend_step == 6U) || (me->backend_step == 8U) ||
                            (me->backend_step == 10U) || (me->backend_step == 12U)) {
                            me->configuration_changed = true;
                        }
                        ++me->backend_step;
                    }
                    me->backend_match = false;
                    if (me->backend_step >= BLE_Backend_CommandCount()) {
                        me->state = BLE_COMPONENT_READY;
                    } else {
                        me->state = BLE_COMPONENT_CONFIGURING;
                        sendBackendStep(me);
                    }
                } else if (strstr(me->line, "ERROR") != (char *)0) {
                    me->failure = BLE_FAILURE_RESPONSE;
                    me->failure_step = me->backend_step;
                    me->state = BLE_COMPONENT_FAILED;
                    me->command_pending = false;
                    traceConfig(me, BLE_FAILURE_RESPONSE);
                }
                else {
                    extern void BLE_Protocol_Line(BleComponent *, char const *);
                    BLE_Backend_ParseLine(me, me->line);
                    if (BLE_Backend_ResponseComplete(me->backend_step, me->line)) {
                        bool const matches = BLE_Backend_ResponseMatches(
                            me->config, me->backend_step, me->line);
                        me->command_pending = false;
                        me->line_len = 0U;
                        me->backend_match = false;
                        if (me->backend_step == 15U) {
                            me->backend_step = BLE_Backend_CommandCount();
                            me->state = BLE_COMPONENT_READY;
                        } else {
                            me->backend_step = (uint8_t)(me->backend_step +
                                (matches ? 2U : 1U));
                            me->state = BLE_COMPONENT_CONFIGURING;
                            sendBackendStep(me);
                        }
                        continue;
                    }
                    BLE_Protocol_Line(me, me->line);
                    me->line_len = 0U;
                }
            }
        } else {
            BLE_Protocol_Consume(me, data[i]);
        }
    }
    if (me->command_pending && elapsed(now, me->deadline_ms)) {
        me->command_pending = false;
        if ((me->backend_step == 14U) &&
            (me->restart_probe_attempts < BLE_COMPONENT_RESTART_PROBE_MAX)) {
            me->restart_waiting = true;
            me->deadline_ms = now + BLE_COMPONENT_RESTART_WAIT_MS;
        } else {
            me->failure = BLE_FAILURE_TIMEOUT;
            me->failure_step = me->backend_step;
            me->state = BLE_COMPONENT_FAILED;
            traceConfig(me, BLE_FAILURE_TIMEOUT);
        }
    }
    if ((me->state == BLE_COMPONENT_READY) && (me->tx_count > 0U)) {
        char *json = me->tx_queue[me->tx_tail];
        if (BLE_Protocol_SendJson(me, json)) {
            (void)strncpy(me->last_json, json, sizeof(me->last_json) - 1U);
            me->last_json[sizeof(me->last_json) - 1U] = '\0';
            me->tx_tail = (uint8_t)((me->tx_tail + 1U) % BLE_COMPONENT_TX_DEPTH);
            --me->tx_count;
        }
    }
}

bool BLE_Component_Report(BleComponent *me, char const *const *names, size_t count) {
    char part[96];
    char value[48];
    size_t used;
    size_t i;
    size_t p;
    int n;
    extern bool BLE_Protocol_Enqueue(BleComponent *, char const *);
    if (!BLE_Component_IsReady(me)) return false;
    n = snprintf(me->work_json, sizeof(me->work_json), "{\"cmd\":\"data\",\"seq\":%u", me->sequence++);
    if ((n < 0) || ((size_t)n >= sizeof(me->work_json))) return false;
    used = (size_t)n;
    for (i = 0U; i < count; ++i) {
        BleParam const *param = (BleParam const *)0;
        for (p = 0U; p < me->param_count; ++p) {
            if (strcmp(me->params[p].name, names[i]) == 0) { param = &me->params[p]; break; }
        }
        if (param == (BleParam const *)0) return false;
        if (param->value.type == BLE_VALUE_INT) n = snprintf(value, sizeof(value), "%ld", (long)param->value.value.integer);
        else if (param->value.type == BLE_VALUE_BOOL) n = snprintf(value, sizeof(value), "%s", param->value.value.boolean ? "true" : "false");
        else if (param->value.type == BLE_VALUE_DECIMAL) {
            int32_t v = param->value.value.decimal.mantissa;
            uint32_t scale = 1U;
            uint8_t d;
            for (d = 0U; d < param->value.value.decimal.decimals; ++d) scale *= 10U;
            n = snprintf(value, sizeof(value), "%s%lu.%02lu", v < 0 ? "-" : "",
                         (unsigned long)((uint32_t)(v < 0 ? -v : v) / scale),
                         (unsigned long)((uint32_t)(v < 0 ? -v : v) % scale));
        } else n = snprintf(value, sizeof(value), "\"%s\"", param->value.value.string);
        if ((n < 0) || ((size_t)n >= sizeof(value))) return false;
        n = snprintf(part, sizeof(part), ",\"%s\":%s", param->name, value);
        if ((n < 0) || ((used + (size_t)n + 2U) >= sizeof(me->work_json))) return false;
        (void)memcpy(&me->work_json[used], part, (size_t)n);
        used += (size_t)n;
        me->work_json[used] = '\0';
    }
    me->work_json[used++] = '}';
    me->work_json[used] = '\0';
    return BLE_Protocol_Enqueue(me, me->work_json);
}

bool BLE_Component_IsConnected(BleComponent const *me) {
    return (me != (BleComponent const *)0) && me->connected;
}

bool BLE_Component_IsReady(BleComponent const *me) {
    return BLE_Component_IsConnected(me) && me->authenticated && !me->reboot_pending;
}
