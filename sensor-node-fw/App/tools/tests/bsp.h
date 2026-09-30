/* 测试替身：BSP 边界，主机 NOR/SPI 模拟；不包含 HAL/QP，不用于固件构建。 */
#ifndef TEST_BSP_H
#define TEST_BSP_H
#include <stdint.h>
#include <stdbool.h>
#include "app_config.h"
typedef enum { BSP_OK, BSP_ERR_PARAM, BSP_ERR_BUSY, BSP_ERR_TIMEOUT, BSP_ERR_IO } bsp_status_t;
#include "bsp_spi.h"
#define BSP_TICKS_PER_SEC (100U)
uint32_t BSP_uptimeMs(void);
bsp_status_t BSP_i2cProbe(uint8_t address, uint32_t timeout);
#endif
