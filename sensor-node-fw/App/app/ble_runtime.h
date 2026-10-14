/* App 层：蓝牙广播窗口与测量参数镜像；无 QP/HAL，便于主机验证。 */
#ifndef APP_BLE_RUNTIME_H
#define APP_BLE_RUNTIME_H
#include "ble_component.h"
typedef struct {
    BleParam params[7];
    uint32_t window_started_ms;
    bool window_open;
    bool session_ready;
    bool data_pending;
    bool battery_valid;
} BleRuntime;
void App_BleRuntime_Init(BleRuntime *runtime);
/** @brief 每次有效按下重新计时，不主动断开已连接的手机。 */
void App_BleRuntime_OpenWindow(BleRuntime *runtime, uint32_t now);
/** @brief 推进广播与发送，返回 true 表示需要发起一次新采样。 */
bool App_BleRuntime_Process(BleRuntime *runtime, BleComponent *component, uint32_t now);
/** @brief 把本轮测量写入 BLE 参数，失败不发送旧值。 */
void App_BleRuntime_Measurement(BleRuntime *runtime, int32_t temperature,
    int32_t x, int32_t y, int32_t z, uint16_t battery, bool battery_valid, bool valid);
#endif
