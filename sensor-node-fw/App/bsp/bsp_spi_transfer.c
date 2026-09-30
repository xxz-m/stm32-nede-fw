/*============================================================================
 * bsp_spi_transfer.c —— BSP 层：共享 SPI1 DMA 事务与所有权
 * 目标硬件：STM32F103RCT6，DMA1 Ch2=SPI1_RX、Ch3=SPI1_TX；所属内核：QV。
 * NOTE (等待)：只检查标志；DMA 完成后先拉高 CS，结果取走后才允许下一设备开始。
 *===========================================================================*/
#include "bsp_spi_internal.h"

static bool l_owned;
static bool l_done;
static BspSpiDevice l_owner;
static bsp_status_t l_result;
static uint32_t l_started;

/** @brief 停止 DMA、恢复空闲 CS；错误时复位 SPI，正常完成时保留模式。 */
static void finish(bsp_status_t result) {
    SPI1->CR2 = 0U;
    DMA1_Channel2->CCR = 0U;
    DMA1_Channel3->CCR = 0U;
    HAL_GPIO_WritePin(BSP_spiDevices[l_owner].port, BSP_spiDevices[l_owner].pin, GPIO_PIN_SET);
    if (result != BSP_OK) {
        /* 不忙等 BSY；复位能终止出错事务，下一事务会重新写模式与分频。 */
        __HAL_RCC_SPI1_FORCE_RESET();
        __HAL_RCC_SPI1_RELEASE_RESET();
    }
    DMA1->IFCR = DMA_IFCR_CGIF2 | DMA_IFCR_CGIF3;
    l_result = result;
    l_done = true;
}

/** @brief 发起一笔 DMA 事务；使用调用者持有的静态缓冲，函数立即返回。 */
bsp_status_t BSP_spiTransferStart(BspSpiDevice device, uint8_t const *tx,
                                 uint8_t *rx, uint16_t length) {
    BspSpiEntry *entry;
    if ((int)device < 0 || device >= BSP_SPI_DEVICE_COUNT || tx == (uint8_t const *)0 ||
        rx == (uint8_t *)0 || length == 0U || length > APP_CFG_SPI_TRANSFER_MAX) {
        return BSP_ERR_PARAM;
    }
    if (l_owned) { return BSP_ERR_BUSY; }
    entry = &BSP_spiDevices[device];
    if (!entry->ready) { return BSP_ERR_PARAM; }
    SPI1->CR1 = entry->cr1; /* 所有已注册设备 CS 均为高，此时才能切模式。 */
    SPI1->CR2 = 0U;
    DMA1_Channel2->CCR = 0U;
    DMA1_Channel3->CCR = 0U;
    DMA1->IFCR = DMA_IFCR_CGIF2 | DMA_IFCR_CGIF3;
    DMA1_Channel2->CPAR = (uint32_t)&SPI1->DR;
    DMA1_Channel3->CPAR = (uint32_t)&SPI1->DR;
    DMA1_Channel2->CMAR = (uint32_t)rx;
    DMA1_Channel3->CMAR = (uint32_t)tx;
    DMA1_Channel2->CNDTR = length;
    DMA1_Channel3->CNDTR = length;
    /* 字节宽度、内存递增、正常模式；TX 的 DIR=1，RX 的 DIR=0，不开 IRQ。 */
    DMA1_Channel2->CCR = DMA_CCR_MINC | DMA_CCR_PL_1;
    DMA1_Channel3->CCR = DMA_CCR_MINC | DMA_CCR_DIR | DMA_CCR_PL_1;
    l_owner = device;
    l_owned = true;
    l_done = false;
    l_started = BSP_uptimeMs();
    HAL_GPIO_WritePin(entry->port, entry->pin, GPIO_PIN_RESET);
    SPI1->CR1 |= SPI_CR1_SPE;
    DMA1_Channel2->CCR |= DMA_CCR_EN;
    DMA1_Channel3->CCR |= DMA_CCR_EN;
    SPI1->CR2 = SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;
    return BSP_OK;
}

/** @brief 按单调毫秒时基检查完成、错误与超时，禁止原地等待。 */
void BSP_spiProcess(void) {
    uint32_t flags;
    if (!l_owned || l_done) { return; }
    flags = DMA1->ISR;
    if ((flags & (DMA_ISR_TEIF2 | DMA_ISR_TEIF3)) != 0U ||
        (SPI1->SR & (SPI_SR_OVR | SPI_SR_MODF)) != 0U) {
        finish(BSP_ERR_IO);
    } else if ((flags & (DMA_ISR_TCIF2 | DMA_ISR_TCIF3)) == (DMA_ISR_TCIF2 | DMA_ISR_TCIF3) &&
               (SPI1->SR & SPI_SR_BSY) == 0U) {
        finish(BSP_OK);
    } else if ((uint32_t)(BSP_uptimeMs() - l_started) >= APP_CFG_SPI_TRANSFER_TIMEOUT_MS) {
        finish(BSP_ERR_TIMEOUT);
    }
}

/** @brief 只有事务所有者可以取走完成结果；不允许另一个设备误解锁总线。 */
bool BSP_spiResult(BspSpiDevice device, bsp_status_t *result) {
    if (!l_owned || !l_done || device != l_owner || result == (bsp_status_t *)0) { return false; }
    *result = l_result;
    l_owned = false;
    return true;
}

/** @brief 查询所有权；未取结果也视为占用，避免接收缓冲被提前复用。 */
bool BSP_spiIsBusy(void) { return l_owned; }
