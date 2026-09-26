/*============================================================================
* ble_port.h —— BLE 组件唯一平台端口
* 层次定位：Component 层；平台通过函数表注入字节流与毫秒时基。
*===========================================================================*/
#ifndef BLE_PORT_H
#define BLE_PORT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    BLE_PORT_OK = 0,
    BLE_PORT_BUSY,
    BLE_PORT_ERROR
} ble_port_status_t;

typedef struct {
    void *context;
    ble_port_status_t (*write)(void *, uint8_t const *, size_t);
    size_t (*read)(void *, uint8_t *, size_t);
    uint32_t (*get_tick_ms)(void *);
    void (*trace)(void *, char const *);
} BlePort;

#endif
