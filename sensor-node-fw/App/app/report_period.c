/* App层：统一业务周期；STM32F103/QV，通过Store既有接口读写持久参数。
 * 无独立计时器；BLE和Modem不得各自维护可写的周期真值。 */
#include "report_period.h"
#include "app_config.h"
#include "store.h"
#include <stdio.h>
uint32_t App_ReportPeriod_Minutes(void) {
    uint32_t minutes;
    if (!SVC_Param_TextToU32(App_Store_Value(SVC_PARAM_TRANSINTVL), &minutes) ||
        minutes < APP_CFG_MODEM_TRANS_INTERVAL_MIN || minutes > APP_CFG_MODEM_TRANS_INTERVAL_MAX) {
        return APP_CFG_MODEM_TRANS_INTERVAL_DEFAULT;
    }
    return minutes;
}
uint32_t App_ReportPeriod_Milliseconds(void) {
    return App_ReportPeriod_Minutes() * 60000U;
}
svc_status_t App_ReportPeriod_SetMinutes(uint32_t minutes) {
    char text[8];
    if (minutes < APP_CFG_MODEM_TRANS_INTERVAL_MIN || minutes > APP_CFG_MODEM_TRANS_INTERVAL_MAX) {
        return SVC_ERR_RANGE;
    }
    (void)snprintf(text, sizeof(text), "%lu", (unsigned long)minutes);
    /* 只受理影子更新；Store AO排队保存，不在BLE回调里等待Flash。 */
    return App_Store_SetName(SVC_Param_Name(SVC_PARAM_TRANSINTVL), text);
}
