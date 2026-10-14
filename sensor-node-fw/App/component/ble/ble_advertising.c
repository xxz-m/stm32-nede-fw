/* Component 层：BLE 运行期广播开关；标准 C + 注入 Port，禁止依赖 QP/HAL。
 * 设置命令不触发复位；已连接时延后执行，保留认证和数据通道。
 * 失败有界重试后进入 FAILED，交给上层恢复；不谎报广播已启停。 */
#include "ble_component.h"
#include "backend/ble_backend.h"
#include <string.h>

void BLE_Component_SetAdvertising(BleComponent *me, bool enabled) {
    if (me->advert_desired != enabled) {
        me->advert_desired = enabled;
        me->advert_attempts = 0U;
        me->advert_retry_ms = me->port->get_tick_ms(me->port->context);
    }
}
/** @brief 故障只重试有限次，期限使用有符号差支持毫秒回绕。 */
static void failed(BleComponent *me, BleFailure reason, uint32_t now) {
    me->advert_pending = false;
    me->advert_retry_ms = now + BLE_COMPONENT_ADVERT_RETRY_MS;
    BLE_TRACE(me, "advertising command failed");
    if (me->advert_attempts >= BLE_COMPONENT_ADVERT_ATTEMPTS) {
        me->failure = reason; me->state = BLE_COMPONENT_FAILED;
    }
}
void BLE_Advertising_Process(BleComponent *me, uint32_t now) {
    char const *command;
    ble_port_status_t status;
    if (me->state != BLE_COMPONENT_READY) { return; }
    if (me->advert_pending) {
        if ((int32_t)(now - me->deadline_ms) >= 0) { failed(me, BLE_FAILURE_TIMEOUT, now); }
        return;
    }
    if (me->connected || me->advert_enabled == me->advert_desired ||
        (int32_t)(now - me->advert_retry_ms) < 0) { return; }
    command = BLE_Backend_AdvertisingCommand(me->advert_desired);
    status = me->port->write(me->port->context, (uint8_t const *)command, strlen(command) + 1U);
    if (status == BLE_PORT_BUSY) { return; }
    if (status != BLE_PORT_OK) {
        ++me->advert_attempts; failed(me, BLE_FAILURE_PORT_WRITE, now); return;
    }
    me->advert_sent = me->advert_desired; me->advert_pending = true;
    me->line_len = 0U; ++me->advert_attempts;
    me->deadline_ms = now + BLE_COMPONENT_TIMEOUT_MS;
}
void BLE_Advertising_Consume(BleComponent *me, uint8_t byte) {
    uint32_t now = me->port->get_tick_ms(me->port->context);
    if (me->line_len < sizeof(me->line) - 1U) { me->line[me->line_len++] = (char)byte; }
    if (byte != '\r' && byte != '\n' && byte != 0U) { return; }
    me->line[me->line_len] = '\0';
    if (strstr(me->line, "OK") != (char *)0) {
        me->advert_enabled = me->advert_sent; me->advert_pending = false; me->advert_attempts = 0U;
        BLE_TRACE(me, me->advert_enabled ? "advertising enabled" : "advertising disabled");
    } else if (strstr(me->line, "+ERP") != (char *)0 || strstr(me->line, "ERROR") != (char *)0) {
        failed(me, BLE_FAILURE_RESPONSE, now);
    } else {
        extern void BLE_Protocol_Line(BleComponent *, char const *);
        BLE_Protocol_Line(me, me->line); /* 连接通知不能被当作 AT 应答吞掉。 */
    }
    me->line_len = 0U;
}
