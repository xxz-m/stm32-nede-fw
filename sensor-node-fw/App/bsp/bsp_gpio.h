/*============================================================================
 * bsp_gpio.h —— BSP 层：板上通用 GPIO 输入
 * 目标硬件：STM32F103RCT6，PB15 外部上拉按键；所属内核：QV。
 * 只暴露逻辑电平，不做消抖、不处理业务、不使用 EXTI。
 *===========================================================================*/
#ifndef BSP_GPIO_H
#define BSP_GPIO_H
#include <stdbool.h>

/** @brief 配置 PB15 为输入、无内部上下拉，保留硬件上拉。 */
void BSP_buttonInit(void);
/** @brief 读取低有效按键；true 表示此刻为低，尚未经消抖。 */
bool BSP_buttonIsPressed(void);
#endif
