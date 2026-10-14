/* App层：统一采集上报周期接口；STM32F103/QV，参数以Store影子为准。
 * BLE与调试/云端AT共用TRANSINTVL，单位分钟，不依赖QP/HAL。 */
#ifndef APP_REPORT_PERIOD_H
#define APP_REPORT_PERIOD_H
#include "svc_frame.h"
#include <stdint.h>
/** @brief 读取当前分钟数；非法旧参数回落出厂值，不访问硬件。 */
uint32_t App_ReportPeriod_Minutes(void);
/** @brief 当前周期毫秒数，供唯一调度者换算时间事件拍数。 */
uint32_t App_ReportPeriod_Milliseconds(void);
/** @brief 校验1~20分钟并修改Store影子，成功立即生效，落盘异步进行。 */
svc_status_t App_ReportPeriod_SetMinutes(uint32_t minutes);
#endif
