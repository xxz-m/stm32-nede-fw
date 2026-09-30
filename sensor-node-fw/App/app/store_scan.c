/* App 层：AT+I2C? 兼容诊断，STM32F103RCT6 / QV；与参数存储介质解耦。 */
#include "store_internal.h"
#include "bsp.h"
#include "svc_log.h"
#include <stdio.h>
static bool l_requested;
static uint8_t l_address, l_count;
/** @brief 受理一次扫描；重复请求不重置当前进度。 */
bool App_Store_ScanRequest(void) {
    if (l_requested) { return false; }
    l_requested = true; l_address = APP_CFG_STORE_SCAN_FIRST; l_count = 0U; return true;
}
/** @brief 扫描仍在等待或推进。 */
bool App_Store_ScanBusy(void) { return l_requested; }
/** @brief 保留原协议和有界探测预算；一次最多四地址、最多八条设备 URC。 */
void App_Store_ScanProcess(void) {
    unsigned burst;
    if (!l_requested) { return; }
    for (burst = 0U; burst < APP_CFG_STORE_SCAN_BURST && l_address <= APP_CFG_STORE_SCAN_LAST; ++burst) {
        bsp_status_t st = BSP_i2cProbe((uint8_t)(l_address << 1), APP_CFG_STORE_PROBE_TIMEOUT_MS);
        if (st == BSP_ERR_BUSY) { return; }
        if (st == BSP_OK && ++l_count <= APP_CFG_STORE_SCAN_URC_MAX) {
            char line[16]; (void)snprintf(line, sizeof(line), "+I2C:%02X\r\n", (unsigned)l_address); LOG_WRITE(line);
        }
        ++l_address;
    }
    if (l_address > APP_CFG_STORE_SCAN_LAST) {
        LOG_WRITE("+I2C:END\r\n"); l_requested = false;
        LOG_I("store: i2c scan done, %u device(s)", (unsigned)l_count);
    }
}
