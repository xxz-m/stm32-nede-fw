/*============================================================================
 * bsp_gpio.c —— BSP 层：PB15 外部上拉按键的 GPIO 封装
 * 目标硬件：STM32F103RCT6；所属内核：QV，仅由 AO_Modem 调用。
 * GPIO 操作集中于本层；不分配中断，不向上层泄漏 HAL 类型。
 *===========================================================================*/
#include "bsp.h"
#include "main.h"

void BSP_buttonInit(void) {
    GPIO_InitTypeDef gpio = {0};
    __HAL_RCC_GPIOB_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_15;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL; /* 外部已有上拉，不叠加内部上拉或下拉。 */
    HAL_GPIO_Init(GPIOB, &gpio);
}

bool BSP_buttonIsPressed(void) {
    return HAL_GPIO_ReadPin(GPIOB, GPIO_PIN_15) == GPIO_PIN_RESET;
}
