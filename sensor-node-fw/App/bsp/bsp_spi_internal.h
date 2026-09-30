/* BSP 层内部：共享 SPI 设备表，STM32F103RCT6 / QV；不向上层暴露寄存器。 */
#ifndef BSP_SPI_INTERNAL_H
#define BSP_SPI_INTERNAL_H
#include "bsp.h"
#include "stm32f1xx.h"
typedef struct { GPIO_TypeDef *port; uint16_t pin; uint16_t cr1; bool ready; } BspSpiEntry;
extern BspSpiEntry BSP_spiDevices[BSP_SPI_DEVICE_COUNT];
#endif
