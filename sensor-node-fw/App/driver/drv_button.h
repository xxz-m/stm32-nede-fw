/*============================================================================
 * drv_button.h —— Driver 层：低有效按键消抖与单次按下识别
 * 目标硬件：BSP 封装的低有效按键；内核无关，不依赖 HAL/QP。
 * 状态由调用方持有；上电按住不触发，稳定松开后才能重新触发。
 *===========================================================================*/
#ifndef DRV_BUTTON_H
#define DRV_BUTTON_H
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t since;
    bool candidate;
    bool stable;
    bool armed;
} DrvButton;

/** @brief 初始化 GPIO 与消抖状态；空指针时不访问硬件。 */
void DRV_Button_Init(DrvButton *button);
/** @brief 周期调用；仅在稳定按下的边沿返回 true，长按不重复。 */
bool DRV_Button_Poll(DrvButton *button);
#endif
