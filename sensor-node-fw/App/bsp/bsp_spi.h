/*============================================================================
 * bsp_spi.h —— BSP 层：SPI1 共享总线接口
 * 目标硬件：STM32F103RCT6；所属内核：QP/C 6.9.1 QV。
 * NOTE (共享)：设备分别配置 CS、模式和分频；一笔 DMA 传输完成并取结果后释放总线。
 * 仅在主循环/AO 中调用，禁止 ISR 调用；缓冲在 Result 取走之前必须保持有效。
 *===========================================================================*/
#ifndef BSP_SPI_H
#define BSP_SPI_H
#include <stdint.h>
#include <stdbool.h>

typedef enum { BSP_SPI_FLASH = 0, BSP_SPI_SENSOR, BSP_SPI_DEVICE_COUNT } BspSpiDevice;
typedef enum { BSP_SPI_PORT_A = 0, BSP_SPI_PORT_B, BSP_SPI_PORT_C } BspSpiPort;
typedef struct {
    BspSpiPort cs_port;
    uint8_t cs_pin;       /*!< 引脚编号 0..15，不暴露 HAL 位掩码 */
    uint8_t mode;         /*!< SPI 模式 0..3 */
    uint16_t divider;     /*!< APB2 时钟分频：2、4、8、16、32、64、128、256 */
} BspSpiConfig;

/** @brief 初始化 SPI1，登记 Flash 与 SCL3300 的片选、模式和分频，不发送器件命令。 */
bsp_status_t BSP_spiInit(void);
/** @brief 注册设备配置；只能空闲时调用，拒绝重复 CS 和 SPI 总线引脚。 */
bsp_status_t BSP_spiDeviceConfigure(BspSpiDevice device, BspSpiConfig const *config);
/** @brief 发起一笔全双工 DMA 事务；同一事务内 CS 始终为低，忙时返回 BUSY。
 * @param tx/rx 发送和接收缓冲，均不得为空；DMA1 通道 2/3 由 SPI1 总线统一持有。
 */
bsp_status_t BSP_spiTransferStart(BspSpiDevice device, uint8_t const *tx,
                                 uint8_t *rx, uint16_t length);
/** @brief 检查 DMA 完成/错误/期限；不等待硬件，无中断和回调。
 * NOTE (两步完成)：本函数停止已完成的 DMA 并拉高 CS，但仍保留设备所有权；
 * 必须由原设备调用 BSP_spiResult() 取走结果，下一笔事务才能开始。
 */
void BSP_spiProcess(void);
/** @brief 取结果并释放所有权；未完成或非所有者返回 false，不会吞掉别人的结果。 */
bool BSP_spiResult(BspSpiDevice device, bsp_status_t *result);
/** @brief 包含尚未取走结果的事务；用于设备配置与作业仲裁。 */
bool BSP_spiIsBusy(void);
#endif
