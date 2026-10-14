/* App 层：按键广播120s、认证首采和统一周期参数镜像；标准 C，不包含 QP/HAL。
 * 已连接的会话继续服务，窗口结束只改变下一次广播许可。 */
#include "ble_runtime.h"
#include "app_config.h"
#include <string.h>
#include "report_period.h"
/** @brief BLE set经注入回调更新共用参数；组件不认识Store或业务事件。 */
static int periodChanged(char const *name, BleValue const *value, void *context) {
    (void)name; (void)context;
    return App_ReportPeriod_SetMinutes((uint32_t)value->value.integer) == SVC_OK ? 0 : -1;
}
void App_BleRuntime_Init(BleRuntime *r) {
    memset(r, 0, sizeof(*r));
    r->params[0] = (BleParam){"angle_x", "Angle X", "deg", {BLE_VALUE_DECIMAL, {.decimal={0,2U}}}, false, -1000,1000,0,0};
    r->params[1] = (BleParam){"temperature", "Temperature", "C", {BLE_VALUE_DECIMAL, {.decimal={0,2U}}}, false,-4000,12500,0,0};
    r->params[2] = (BleParam){"angle_y", "Angle Y", "deg", {BLE_VALUE_DECIMAL, {.decimal={0,2U}}}, false,-1000,1000,0,0};
    r->params[3] = (BleParam){"angle_z", "Angle Z", "deg", {BLE_VALUE_DECIMAL, {.decimal={0,2U}}}, false,-1000,1000,0,0};
    r->params[4] = (BleParam){"signal_quality", "Signal quality", "CSQ", {BLE_VALUE_INT, {.integer=99}}, false,0,99,0,0};
    r->params[5] = (BleParam){"voltage", "Voltage", "mV", {BLE_VALUE_INT, {.integer=0}}, false,0,5000,0,0};
    r->params[6] = (BleParam){"sample_interval", "Report interval", "min", {BLE_VALUE_INT, {.integer=APP_CFG_MODEM_TRANS_INTERVAL_DEFAULT}}, true,APP_CFG_MODEM_TRANS_INTERVAL_MIN,APP_CFG_MODEM_TRANS_INTERVAL_MAX,periodChanged,0};
}
void App_BleRuntime_OpenWindow(BleRuntime *r, uint32_t now) {
    r->window_open = true; r->window_started_ms = now;
}
/** @brief 电压无效时省略字段，不保留固定 3300mV 或伪造零电压。 */
static bool sendData(BleRuntime *r, BleComponent *c) {
    char const *names[7] = {"angle_x","temperature","angle_y","angle_z","signal_quality","sample_interval","voltage"};
    return BLE_Component_Report(c, names, r->battery_valid ? 7U : 6U);
}
bool App_BleRuntime_Process(BleRuntime *r, BleComponent *c, uint32_t now) {
    if (r->window_open && (uint32_t)(now - r->window_started_ms) >= APP_CFG_BLE_ADVERT_WINDOW_MS) {
        r->window_open = false;
    }
    BLE_Component_SetAdvertising(c, r->window_open);
    if (!BLE_Component_IsReady(c)) {
        r->session_ready = false; r->data_pending = false;
        return false;
    }
    if (r->data_pending && sendData(r,c)) { r->data_pending = false; }
    if (!r->session_ready) {
        r->session_ready = true;
        return true; /* 第一帧必须来自新采样，不能先把参数初值发送出去。 */
    }
    /* 周期只由统一调度者推进，避免BLE和4G各采一次、各报一次。 */
    return false;
}
void App_BleRuntime_Measurement(BleRuntime *r, int32_t temperature,
    int32_t x, int32_t y, int32_t z, uint16_t battery, bool battery_valid, bool valid) {
    if (!valid) { r->data_pending = false; return; }
    r->params[0].value.value.decimal.mantissa = x;
    r->params[1].value.value.decimal.mantissa = temperature;
    r->params[2].value.value.decimal.mantissa = y;
    r->params[3].value.value.decimal.mantissa = z;
    r->params[5].value.value.integer = battery;
    r->battery_valid = battery_valid; r->data_pending = true;
}
