/*============================================================================
* ble_component_config.h —— 可移植 BLE 组件编译期配置
* 层次定位：Component 层；仅使用标准 C，不依赖 HAL/QP/RTOS。
*===========================================================================*/
#ifndef BLE_COMPONENT_CONFIG_H
#define BLE_COMPONENT_CONFIG_H

#include "app_config.h"

/* NOTE：本文件是工程配置到可移植组件命名空间的唯一映射点。 */
#define BLE_BACKEND_VG6328                 APP_CFG_BLE_BACKEND_VG6328
#define BLE_COMPONENT_BACKEND              APP_CFG_BLE_BACKEND
#define BLE_COMPONENT_JSON_MAX             APP_CFG_BLE_JSON_MAX
#define BLE_COMPONENT_MAX_PARAMS           APP_CFG_BLE_MAX_PARAMS
#define BLE_COMPONENT_TX_DEPTH             APP_CFG_BLE_TX_DEPTH
#define BLE_COMPONENT_FRAME_MAX            APP_CFG_BLE_FRAME_MAX
#define BLE_COMPONENT_FRAGMENT_DATA_MAX    APP_CFG_BLE_FRAGMENT_DATA_MAX
#define BLE_COMPONENT_RX_BUDGET            APP_CFG_BLE_RX_BUDGET
#define BLE_COMPONENT_TIMEOUT_MS           APP_CFG_BLE_COMMAND_TIMEOUT_MS
#define BLE_COMPONENT_RESTART_WAIT_MS      APP_CFG_BLE_RESTART_WAIT_MS
#define BLE_COMPONENT_RESTART_PROBE_MAX    APP_CFG_BLE_RESTART_PROBE_ATTEMPTS
#define BLE_COMPONENT_TRACE_ENABLE         APP_CFG_BLE_TRACE_ENABLE

#if (BLE_COMPONENT_TRACE_ENABLE != 0U)
#define BLE_TRACE(me_, text_) do {                                      \
    if (((me_)->port != (BlePort const *)0) &&                          \
        ((me_)->port->trace != (void (*)(void *, char const *))0)) {    \
        (me_)->port->trace((me_)->port->context, (text_));              \
    }                                                                   \
} while (0)
#else
#define BLE_TRACE(me_, text_) ((void)0)
#endif

#endif
