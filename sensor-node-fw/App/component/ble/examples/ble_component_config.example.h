/*============================================================================
* ble_component_config.h —— 独立工程配置模板
* 用法：复制并覆盖交付包根目录的 ble_component_config.h，然后按产品修改。
*===========================================================================*/
#ifndef BLE_COMPONENT_CONFIG_H
#define BLE_COMPONENT_CONFIG_H

#define BLE_BACKEND_VG6328                 (1U)
#define BLE_COMPONENT_BACKEND              (BLE_BACKEND_VG6328)

#define BLE_COMPONENT_JSON_MAX             (768U)
#define BLE_COMPONENT_MAX_PARAMS           (8U)
#define BLE_COMPONENT_TX_DEPTH             (2U)
#define BLE_COMPONENT_FRAME_MAX            (20U)
#define BLE_COMPONENT_FRAGMENT_DATA_MAX    (14U)
#define BLE_COMPONENT_RX_BUDGET            (64U)

#define BLE_COMPONENT_TIMEOUT_MS           (1000U)
#define BLE_COMPONENT_RESTART_WAIT_MS      (500U)
#define BLE_COMPONENT_RESTART_PROBE_MAX    (6U)

/* 1：调用 BlePort.trace；0：编译期移除组件 trace。 */
#define BLE_COMPONENT_TRACE_ENABLE         (1U)

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
