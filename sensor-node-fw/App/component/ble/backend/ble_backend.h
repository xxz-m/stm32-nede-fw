/*============================================================================
* ble_backend.h —— BLE 芯片后端内部接口
*===========================================================================*/
#ifndef BLE_BACKEND_H
#define BLE_BACKEND_H

#include "ble_component.h"

char const *BLE_Backend_Command(BleConfig const *config, uint8_t step,
                                char *buffer, size_t capacity);
uint8_t BLE_Backend_CommandCount(void);
bool BLE_Backend_LineConnected(char const *line);
bool BLE_Backend_LineDisconnected(char const *line);
void BLE_Backend_ParseLine(BleComponent *component, char const *line);
bool BLE_Backend_ResponseMatches(BleConfig const *config, uint8_t step,
                                 char const *line);
bool BLE_Backend_ResponseComplete(uint8_t step, char const *line);

#endif
