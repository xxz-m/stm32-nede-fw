/*============================================================================
* ble_component.h —— 芯片无关 BLE 业务组件公共接口
* 层次定位：Component 层；保持小程序协议稳定，具体芯片由后端适配。
*===========================================================================*/
#ifndef BLE_COMPONENT_H
#define BLE_COMPONENT_H

#include "ble_component_config.h"
#include "ble_port.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    BLE_VALUE_BOOL = 0,
    BLE_VALUE_INT,
    BLE_VALUE_DECIMAL,
    BLE_VALUE_STRING
} BleValueType;

typedef struct {
    BleValueType type;
    union {
        bool boolean;
        int32_t integer;
        struct { int32_t mantissa; uint8_t decimals; } decimal;
        char const *string;
    } value;
} BleValue;

typedef int (*BleParamChanged)(char const *, BleValue const *, void *);

typedef struct {
    char const *name;
    char const *label;
    char const *unit;
    BleValue value;
    bool writable;
    int32_t minimum;
    int32_t maximum;
    BleParamChanged changed;
    void *context;
} BleParam;

typedef struct {
    char const *serial_number;
    char const *pin;
    char const *local_name;
    char const *service_uuid;
    char const *device_name;
    char const *model_code;
    char const *batch_code;
} BleConfig;

typedef enum {
    BLE_COMPONENT_STOPPED = 0,
    BLE_COMPONENT_PROBING,
    BLE_COMPONENT_CONFIGURING,
    BLE_COMPONENT_READY,
    BLE_COMPONENT_FAILED
} BleComponentState;

typedef enum {
    BLE_FAILURE_NONE = 0,
    BLE_FAILURE_PORT_WRITE,
    BLE_FAILURE_TIMEOUT,
    BLE_FAILURE_RESPONSE
} BleFailure;

typedef struct {
    BlePort const *port;
    BleConfig const *config;
    BleParam *params;
    size_t param_count;
    BleComponentState state;
    uint8_t backend_step;
    bool command_pending;
    bool connected;
    bool authenticated;
    bool reboot_pending;
    bool backend_match;
    bool configuration_changed;
    bool restart_waiting;
    uint8_t restart_probe_attempts;
    BleFailure failure;
    uint8_t failure_step;
    uint8_t sequence;
    uint8_t rx_frame[BLE_COMPONENT_FRAME_MAX * 2U];
    size_t rx_frame_len;
    char rx_json[BLE_COMPONENT_JSON_MAX + 1U];
    char work_json[BLE_COMPONENT_JSON_MAX + 1U];
    size_t rx_json_len;
    uint16_t rx_total;
    uint8_t rx_next_sequence;
    char line[96];
    size_t line_len;
    char mac_address[18];
    char tx_queue[BLE_COMPONENT_TX_DEPTH][BLE_COMPONENT_JSON_MAX + 1U];
    uint8_t tx_head;
    uint8_t tx_tail;
    uint8_t tx_count;
    char last_json[BLE_COMPONENT_JSON_MAX + 1U];
    uint32_t deadline_ms;
} BleComponent;

void BLE_Component_Init(BleComponent *, BlePort const *, BleConfig const *, BleParam *, size_t);
void BLE_Component_Start(BleComponent *);
void BLE_Component_Process(BleComponent *);
bool BLE_Component_Report(BleComponent *, char const *const *, size_t);
bool BLE_Component_IsConnected(BleComponent const *);
bool BLE_Component_IsReady(BleComponent const *);

#endif
