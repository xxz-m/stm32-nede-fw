/*============================================================================
 * bsp_spi.c —— BSP 层：SPI1 初始化与设备配置
 * 目标硬件：STM32F103RCT6，PA5/PA6/PA7；Flash CS=PA8，SCL3300 CS=PA4。
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

/**
 * @brief 为一个逻辑 SPI 设备登记片选、通信模式和分频，并初始化它的 CS 引脚。
 * @param device 软件设备编号，同时作为 BSP_spiDevices[] 的下标；不是 SPI 线上发送的地址
 * @param config 配置只读；函数把所需字段转换后保存，不保留该指针
 * @return BSP_OK = 登记成功；BSP_ERR_PARAM = 配置非法；BSP_ERR_BUSY = 总线仍被占用
 *
 * 例如 SCL3300：device=BSP_SPI_SENSOR，端口 A、引脚 4、Mode 0、分频 32。
 * 登记后得到 GPIOA、GPIO_PIN_4 和预先组合好的 CR1 配置；发送事务时再按设备编号取用。
 * NOTE (职责)：此函数配置 CS 并保存设备参数，不拉低 CS、不启动 DMA，也不发送器件命令。
 * SPI1->CR1 的实际写入由 BSP_spiTransferStart() 在每笔事务开始时完成。
 */
bsp_status_t BSP_spiDeviceConfigure(BspSpiDevice device, BspSpiConfig const *config) {
    /* 只初始化要用的 GPIO 字段；其余字段清零，避免带入栈上的未定义值。 */
    GPIO_InitTypeDef gpio = {0};
    GPIO_TypeDef *port;       /* 由端口编号解析出的 GPIOA / GPIOB / GPIOC 地址 */
    uint16_t div;             /* 分频副本，用来计算编码，不修改调用者的配置 */
    uint32_t br = 0U;         /* SPI_CR1.BR 的三位编码，不是实际分频数 */
    unsigned i;              /* 遍历设备表，检查是否与其他设备共用了同一个 CS */

    /* 1. 检查设备下标、空指针、引脚编号和 SPI 模式。
     * STM32 每组 GPIO 的引脚编号为 0..15；SPI 模式为 0..3。
     * NOTE (短路求值)：空指针判断放在 config->... 前面，空指针不会被解引用。 */
    if ((device >= BSP_SPI_DEVICE_COUNT) || ((int)device < 0) ||
        (config == (BspSpiConfig const *)0) || config->cs_pin > 15U || config->mode > 3U) {
        return BSP_ERR_PARAM;
    }
    /* 2. 只有总线完全空闲时才允许改设备配置。
     * DMA 已结束但原设备尚未取走结果，也算占用，避免事务中途改变 CS 或配置。 */
    if (BSP_spiIsBusy()) { return BSP_ERR_BUSY; }

    /* 3. STM32F1 的 SPI 分频只能为 2、4、8、16、32、64、128、256。
     * 正整数 x 若是 2 的幂，二进制只有一位为 1，因此 x & (x-1) 必为 0。
     * 范围检查再排除 0、1 以及大于 256 的值。 */
    div = config->divider;
    if (div < 2U || div > 256U || (div & (div - 1U)) != 0U) { return BSP_ERR_PARAM; }

    /* 4. 把分频数换成 BR 编码：/2→0、/4→1、/8→2，依次到 /256→7。
     * 例如 /32：32→16→8→4→2，右移四次，所以 br=4，即二进制 100。
     * 循环只做最多七次整数运算，不等待硬件。 */
    while (div > 2U) { div >>= 1; ++br; }

    /* 5. 当前板子的 SPI1 使用 PA5=SCK、PA6=MISO、PA7=MOSI。
     * 这些引脚不能同时用作从机片选，否则后面的 GPIO 输出配置会破坏总线功能。 */
    if (config->cs_port == BSP_SPI_PORT_A && config->cs_pin >= 5U && config->cs_pin <= 7U) {
        return BSP_ERR_PARAM;
    }
    /* 6. 把 BSP 的端口编号转换成 HAL 使用的 GPIO 地址，并开启对应 GPIO 时钟。
     * 上层只需传 A/B/C 编号；HAL 类型和寄存器地址留在 BSP 内部。 */
    port = gpioPort(config->cs_port);
    if (port == (GPIO_TypeDef *)0) { return BSP_ERR_PARAM; }

    /* 7. 检查该 CS 是否已分配给另一个设备。
     * 比较时必须同时匹配端口和引脚：PA4 与 PB4 是两个不同的引脚。
     * 跳过当前 device，允许空闲时重新登记同一设备；未 ready 的槽位尚未有效登记。
     * 若两个设备使用同一个 CS，选中其中一个时会同时选中另一个，因此拒绝配置。 */
    for (i = 0U; i < BSP_SPI_DEVICE_COUNT; ++i) {
        if (i != (unsigned)device && BSP_spiDevices[i].ready &&
            BSP_spiDevices[i].port == port && BSP_spiDevices[i].pin == (1U << config->cs_pin)) {
            return BSP_ERR_PARAM;
        }
    }
    /* 8. 把引脚编号转换成 HAL 要求的位掩码：例如 pin=4 → 1<<4 → 0x0010。
     * CS 低有效，空闲应为高；先把输出锁存值写高，再把引脚切换成输出，
     * 避免切换瞬间输出低电平，造成一次意外选中。 */
    gpio.Pin = 1U << config->cs_pin;
    HAL_GPIO_WritePin(port, (uint16_t)gpio.Pin, GPIO_PIN_SET);
    gpio.Mode = GPIO_MODE_OUTPUT_PP;      /* 普通推挽输出，由软件控制 CS，不用 SPI 硬件 NSS */
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;    /* GPIO 输出速度等级，不是 SPI 的 SCK 频率 */
    HAL_GPIO_Init(port, &gpio);

    /* 9. 保存 CS 的实际端口和位掩码；发送事务时据此拉低，传输结束时据此拉高。 */
    BSP_spiDevices[device].port = port;
    BSP_spiDevices[device].pin = (uint16_t)gpio.Pin;

    /* 10. 预先组合该设备的 SPI_CR1 配置，保存在软件表中，此处不写 SPI 寄存器。
     * MSTR：主机模式；SSM：软件管理内部 NSS；SSI：内部 NSS 保持高电平。
     * NOTE (NSS 与 CS)：SSI 不是外部 CS 引脚电平，实际从机选择仍由上方 GPIO 控制。
     * BR 位于 CR1[5:3]，因此编码 br 左移 3 位；/32 的 br=4，移位后为 0x0020。
     * CPOL/CPHA 位于 CR1[1:0]，刚好对应 mode 的两位，可直接按位或：
     *   Mode 0=00、Mode 1=01、Mode 2=10、Mode 3=11（顺序为 CPOL、CPHA）。
     * 不同从机可以保存不同的模式和分频，下一次开始事务时按 device 选择配置。 */
    BSP_spiDevices[device].cr1 = (uint16_t)(SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI |
                                            (br << 3) | config->mode);
    /* 11. 最后标记配置可用；ready 只代表槽位配置完成，不代表已检测到真实器件。 */
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
