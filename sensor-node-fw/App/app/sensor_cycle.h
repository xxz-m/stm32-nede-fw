/* App 层：统一的非阻塞采样作业；STM32F103/SCL3300/电池，QV 主上下文使用。
 * 只管理作业，不认识通信 AO 或发送协议；请求按来源位合并，结果按值交接。 */
#ifndef APP_SENSOR_CYCLE_H
#define APP_SENSOR_CYCLE_H
#include "drv_scl3300.h"
#include "svc_filter.h"
#include <stdbool.h>
#include <stdint.h>
#define SENSOR_TARGET_CLOUD (1U)
#define SENSOR_TARGET_BLE   (2U)
typedef struct {
    DrvSensorSample sensor;
    uint16_t battery_mV;
    uint8_t targets;
    bool valid;
    bool battery_valid;
} SensorCycleResult;
typedef struct {
    SvcMavg filter[DRV_SENSOR_AXIS_COUNT];
    SensorCycleResult result;
    uint32_t started_ms;
    uint8_t pending;
    uint8_t count;
    bool active;
    bool done;
    bool sensor_pending;
    bool battery_started;
    bool battery_done;
} SensorCycle;
/** @brief 清空作业；器件初始化由 AO 启动入口完成。 */
void App_SensorCycle_Init(SensorCycle *cycle);
/** @brief 合并云端/蓝牙请求；进行中的新鲜采样可同时服务两个来源。 */
void App_SensorCycle_Request(SensorCycle *cycle, uint8_t targets);
/** @brief 每 20ms 推进一步，初始化与器件恢复也持续推进。 */
void App_SensorCycle_Process(SensorCycle *cycle);
/** @brief 取走成功或失败终态；没有终态返回 false。 */
bool App_SensorCycle_Take(SensorCycle *cycle, SensorCycleResult *result);
/** @brief 取消电池作业与业务请求，残留 SPI 结果由后续推进收取。 */
void App_SensorCycle_Cancel(SensorCycle *cycle);
#endif
