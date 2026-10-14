/*============================================================================
* ble_component_config.h —— 可移植 BLE 组件编译期配置
* 层次定位：Component 层；仅使用标准 C，不依赖 HAL/QP/RTOS。
*===========================================================================*/
#ifndef BLE_COMPONENT_CONFIG_H
#define BLE_COMPONENT_CONFIG_H

#include "app_config.h"
#define BLE_COMPONENT_ADVERT_RETRY_MS APP_CFG_BLE_ADVERT_RETRY_MS
#define BLE_COMPONENT_ADVERT_ATTEMPTS APP_CFG_BLE_ADVERT_ATTEMPTS

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
/*! 把**模组发来的每一行**原样打进日志（临时诊断开关，2026-09-28 加）。
*  NOTE (与 4G 侧同款)：4G 那边有 `APP_CFG_MODEM_TRACE_URC`，当初就是靠它抓出
*    「订阅成功但订错主题」（`#51`）。BLE 侧此前**没有**对应开关：`BLE_Protocol_Line()`
*    对认不出的行只做自己的两件事（连接/断开），其余**静默丢弃**，于是
*    「手机根本没连上」与「连上了但模块的话我们不认识」在现场无法区分。 */
#define BLE_COMPONENT_TRACE_RX_ENABLE      APP_CFG_BLE_TRACE_RX

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
