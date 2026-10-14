/*============================================================================
 * modem_report.c —— App 层：业务事件请求一次 4G 属性上报
 * 目标硬件：经组件访问的 4G 模组；所属内核：QV，非阻塞。
 * 不依赖按键 Driver；只决定何时交给组件，日志不代表云端已收到。
 *===========================================================================*/
#include "modem_report.h"
#include "svc_log.h"

void App_ModemReport_Init(ModemReportRequest *request) {
    request->pending = false;
}

void App_ModemReport_Request(ModemReportRequest *request) {
    /* 合并为一条待发请求，避免离线或长时间忙时无限积压。 */
    request->pending = true;
    LOG_I("modem: report requested");
}

void App_ModemReport_TrySend(ModemReportRequest *request, ModemComponent *component,
                               bool measurement_valid) {
    if (!request->pending || !measurement_valid || !Modem_Component_IsReady(component) ||
        !component->report_enabled || component->report_requested ||
        component->runtime_op == MODEM_RUNTIME_REPORT) {
        return;
    }
    /* 不能在上一帧在途时置位：其完成路径会清除 report_requested，丢掉新请求。 */
    Modem_Component_RequestReport(component);
    request->pending = false;
    LOG_I("modem: report queued");
}
