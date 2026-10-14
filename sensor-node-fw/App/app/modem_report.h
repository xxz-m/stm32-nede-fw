/*============================================================================
 * modem_report.h —— App 层：一次性 4G 上报请求的等待与合并
 * 目标硬件：经可移植组件访问的 4G 模组；所属内核：QV，状态仅归 AO_Modem。
 * 不认识按键或 GPIO；任何 App 业务事件均可请求，复用原有上报接口。
 *===========================================================================*/
#ifndef APP_MODEM_REPORT_H
#define APP_MODEM_REPORT_H
#include "modem_component.h"

typedef struct {
    bool pending;
} ModemReportRequest;

/** @brief 初始化最多一条待发请求，不访问 GPIO。 */
void App_ModemReport_Init(ModemReportRequest *request);
/** @brief 记录业务事件的上报请求；多次请求合并，离线时保留。 */
void App_ModemReport_Request(ModemReportRequest *request);
/** @brief 在线且已有测量时排队；不改定时周期、不绕过停报开关。 */
void App_ModemReport_TrySend(ModemReportRequest *request, ModemComponent *component,
                               bool measurement_valid);
#endif
