/*============================================================================
 * drv_button.c —— Driver 层：基于时间的按键消抖
 * 目标硬件：BSP 封装的低有效输入；内核无关，不忙等。
 * 由调用方周期推进，按下和松开都需稳定，毫秒差兼容计数回绕。
 *===========================================================================*/
#include "drv_button.h"
#include "bsp.h"

void DRV_Button_Init(DrvButton *button) {
    if (button == (DrvButton *)0) { return; }
    BSP_buttonInit();
    button->candidate = BSP_buttonIsPressed();
    button->stable = button->candidate;
    button->armed = !button->stable;
    button->since = BSP_uptimeMs();
}

bool DRV_Button_Poll(DrvButton *button) {
    bool pressed;
    uint32_t now;
    if (button == (DrvButton *)0) { return false; }
    pressed = BSP_buttonIsPressed();
    now = BSP_uptimeMs();
    if (pressed != button->candidate) {
        button->candidate = pressed;
        button->since = now;
        return false;
    }
    if (pressed == button->stable || now - button->since < APP_CFG_BUTTON_DEBOUNCE_MS) {
        return false;
    }
    button->stable = pressed;
    if (!pressed) {
        button->armed = true;
        return false;
    }
    if (button->armed) {
        button->armed = false;
        return true;
    }
    return false;
}
