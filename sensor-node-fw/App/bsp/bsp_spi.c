/*============================================================================
 * bsp_spi.c —— BSP 层：SPI1 初始化与设备配置
 * 目标硬件：STM32F103RCT6，PA5/PA6/PA7；Flash CS=PA8，传感器 CS 待确认。
 * 所属内核：QP/C 6.9.1 QV；模式在全部 CS 为高、总线空闲时切换。
 * NOTE (再生成)：基线配置登记在 .ioc；BSP 重申 DMA 通道与设备配置，不依赖生成句柄。
 *===========================================================================*/
#include "bsp_spi_internal.h"

BspSpiEntry BSP_spiDevices[BSP_SPI_DEVICE_COUNT];

/** @brief 根据 BSP 端口编号开启 GPIO 时钟并解析地址。 */
static GPIO_TypeDef *gpioPort(BspSpiPort port) {
    switch (port) {
        case BSP_SPI_PORT_A: __HAL_RCC_GPIOA_CLK_ENABLE(); return GPIOA;
        case BSP_SPI_PORT_B: __HAL_RCC_GPIOB_CLK_ENABLE(); return GPIOB;
        case BSP_SPI_PORT_C: __HAL_RCC_GPIOC_CLK_ENABLE(); return GPIOC;
        default: return (GPIO_TypeDef *)0;
    }
}

/** @brief 验证设备配置并建立独立的 CS、模式和分频；无总线传输。 */
bsp_status_t BSP_spiDeviceConfigure(BspSpiDevice device, BspSpiConfig const *config) {
    GPIO_InitTypeDef gpio = {0};
    GPIO_TypeDef *port;
    uint16_t div;
    uint32_t br = 0U;
    unsigned i;
    if ((device >= BSP_SPI_DEVICE_COUNT) || ((int)device < 0) ||
        (config == (BspSpiConfig const *)0) || config->cs_pin > 15U || config->mode > 3U) {
        return BSP_ERR_PARAM;
    }
    if (BSP_spiIsBusy()) { return BSP_ERR_BUSY; }
    div = config->divider;
    if (div < 2U || div > 256U || (div & (div - 1U)) != 0U) { return BSP_ERR_PARAM; }
    while (div > 2U) { div >>= 1; ++br; }
    if (config->cs_port == BSP_SPI_PORT_A && config->cs_pin >= 5U && config->cs_pin <= 7U) {
        return BSP_ERR_PARAM;
    }
    port = gpioPort(config->cs_port);
    if (port == (GPIO_TypeDef *)0) { return BSP_ERR_PARAM; }
    for (i = 0U; i < BSP_SPI_DEVICE_COUNT; ++i) {
        if (i != (unsigned)device && BSP_spiDevices[i].ready &&
            BSP_spiDevices[i].port == port && BSP_spiDevices[i].pin == (1U << config->cs_pin)) {
            return BSP_ERR_PARAM;
        }
    }
    /* 先写高再配置输出，避免 GPIO 模式切换产生一次假片选。 */
    gpio.Pin = 1U << config->cs_pin;
    HAL_GPIO_WritePin(port, (uint16_t)gpio.Pin, GPIO_PIN_SET);
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(port, &gpio);
    BSP_spiDevices[device].port = port;
    BSP_spiDevices[device].pin = (uint16_t)gpio.Pin;
    /* NOTE (CR1)：主机、软件 NSS 且内部 NSS 高；BR 位 5:3，CPOL/CPHA 位 1:0。 */
    BSP_spiDevices[device].cr1 = (uint16_t)(SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                                            (br << 3) | config->mode);
    BSP_spiDevices[device].ready = true;
    return BSP_OK;
}

/** @brief 初始化共享 SPI1；DMA 正常模式由事务模块配置，不使能 DMA/SPI IRQ。 */
bsp_status_t BSP_spiInit(void) {
    GPIO_InitTypeDef gpio = {0};
    BspSpiConfig const flash = { BSP_SPI_PORT_A, APP_CFG_FLASH_CS_PIN,
                                 APP_CFG_FLASH_SPI_MODE, APP_CFG_FLASH_SPI_DIVIDER };
    /* SCL3300-D01：片选 PA4（用户 2026-10-10 确认），模式 0、APB2/32 = 2.25 MHz。
     * 与 Flash 同为 Mode 0，因此共享 SPI1 时不需要在事务间切换模式，
     * 只由事务模块按设备重写 CR1 的分频位。 */
    BspSpiConfig const sensor = { (BspSpiPort)APP_CFG_SCL3300_CS_PORT, APP_CFG_SCL3300_CS_PIN,
                                  APP_CFG_SCL3300_SPI_MODE, APP_CFG_SCL3300_SPI_DIVIDER };
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();
    __HAL_RCC_SPI1_FORCE_RESET();
    __HAL_RCC_SPI1_RELEASE_RESET();
    /* 先释放 Flash CS，再切换 SCK/MOSI 复用，避免初始化边沿被误当成指令。 */
    if (BSP_spiDeviceConfigure(BSP_SPI_FLASH, &flash) != BSP_OK) { return BSP_ERR_PARAM; }
    /* 传感器的片选同样是「先写高再配输出」，且在 SCK/MOSI 复用之前完成，
     * 保证复用切换产生的边沿不会被 SCL3300 当成一帧。 */
    if (BSP_spiDeviceConfigure(BSP_SPI_SENSOR, &sensor) != BSP_OK) { return BSP_ERR_PARAM; }
    gpio.Pin = GPIO_PIN_5 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);
    SPI1->CR2 = 0U;
    return BSP_OK;
}
