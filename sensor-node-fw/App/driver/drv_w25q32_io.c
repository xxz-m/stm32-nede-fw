/*============================================================================
 * drv_w25q32_io.c —— Driver 层：W25Q32JV 单条指令封装
 * 目标器件：W25Q32JVSNIQ；内核无关。SPI 事务仅在传输时持有总线，BUSY 等待不持锁。
 *===========================================================================*/
#include "drv_w25q32.h"
#include "bsp.h"
#include <string.h>

static uint8_t l_tx[APP_CFG_SPI_TRANSFER_MAX];
static uint8_t l_rx[APP_CFG_SPI_TRANSFER_MAX];
static uint16_t l_header;

/** @brief 构造指令并发起 DMA；忙时不覆盖当前事务缓冲。 */
drv_status_t DRV_W25Q32_IoStart(uint8_t cmd, uint32_t addr, bool addressed,
                               uint8_t const *data, uint16_t length) {
    uint16_t header = addressed ? 4U : 1U;
    bsp_status_t st;
    if (length > APP_CFG_SPI_TRANSFER_MAX - header) { return DRV_ERR_PARAM; }
    if (BSP_spiIsBusy()) { return DRV_ERR_BUSY; }
    memset(l_tx, 0xFF, (size_t)header + length);
    l_tx[0] = cmd;
    if (addressed) {
        l_tx[1] = (uint8_t)(addr >> 16);
        l_tx[2] = (uint8_t)(addr >> 8);
        l_tx[3] = (uint8_t)addr;
    }
    if (data != (uint8_t const *)0) { memcpy(l_tx + header, data, length); }
    st = BSP_spiTransferStart(BSP_SPI_FLASH, l_tx, l_rx, (uint16_t)(header + length));
    if (st == BSP_OK) { l_header = header; return DRV_OK; }
    return st == BSP_ERR_BUSY ? DRV_ERR_BUSY : DRV_ERR_BUS;
}

/** @brief 取本设备事务结果，并释放 SPI 所有权，不会取走传感器结果。 */
bool DRV_W25Q32_IoResult(drv_status_t *result) {
    bsp_status_t st;
    BSP_spiProcess();
    if (!BSP_spiResult(BSP_SPI_FLASH, &st)) { return false; }
    *result = st == BSP_OK ? DRV_OK : (st == BSP_ERR_TIMEOUT ? DRV_ERR_TIMEOUT : DRV_ERR_BUS);
    return true;
}

/** @brief 返回命令/地址后的接收载荷，只在完成后读取，下一命令将覆盖它。 */
uint8_t const *DRV_W25Q32_IoData(void) { return l_rx + l_header; }
