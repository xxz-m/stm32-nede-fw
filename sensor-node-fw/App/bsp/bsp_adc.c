/*============================================================================
 * bsp_adc.c —— BSP 层：ADC1 寄存器封装、非阻塞校准与电池开关
 * 目标硬件：STM32F103RCT6；所属内核：QV；无新增中断或 DMA 通道。
 * 来源：ST RM0008 ADC 章节；所有等待由调用方周期推进，禁止原地轮询。
 *===========================================================================*/
#include "bsp.h"
#include "main.h"

typedef enum {
    ADC_OFF, ADC_POWER, ADC_RESET_CAL, ADC_CAL, ADC_IDLE,
    ADC_WAKE, ADC_CONVERT, ADC_DONE, ADC_FAULT
} AdcState;
static AdcState l_state;
static uint32_t l_since;
static uint16_t l_raw;
static bsp_status_t l_result;
static bool l_calibrated;
static bool l_initialized;

/** @brief 控制 PC1；先写低再切输出，避免产生一次推挽高电平。 */
void BSP_batteryGateSet(bool enabled) {
    GPIO_InitTypeDef gpio = {0};
#if (APP_CFG_BATTERY_GATE_HOLD_LOW != 0U)
    /* NOTE (临时测试)：用户要求持续开启分压，完成/失败/取消也不释放 PC1。 */
    enabled = true;
#endif
    __HAL_RCC_GPIOC_CLK_ENABLE();
    gpio.Pin = GPIO_PIN_1;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    if (enabled) {
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_1, GPIO_PIN_RESET);
        gpio.Mode = GPIO_MODE_OUTPUT_OD;
    } else {
        /* NOTE (硬件)：高阻不能隔离 4.2 V；PC1 非 FT，现板风险见资源文档。 */
        gpio.Mode = GPIO_MODE_ANALOG;
    }
    HAL_GPIO_Init(GPIOC, &gpio);
}

/** @brief 保存终态；错误时关 ADC，仍保留待取结果。 */
static void finish(bsp_status_t result) {
    l_result = result;
    if (result != BSP_OK) {
        ADC1->CR2 &= ~(ADC_CR2_ADON | ADC_CR2_TSVREFE | ADC_CR2_EXTTRIG);
    }
    l_state = ADC_DONE;
}

bsp_status_t BSP_adcInit(void) {
    GPIO_InitTypeDef gpio = {0};
    if (l_initialized && l_state != ADC_IDLE && l_state != ADC_FAULT) { return BSP_ERR_BUSY; }
    BSP_batteryGateSet(false);
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_ADC1_FORCE_RESET();
    __HAL_RCC_ADC1_RELEASE_RESET();
    /* NOTE (时钟)：与 .ioc 的 ADCPCLK2_DIV6 一致，72/6=12 MHz，不超过 14 MHz。 */
    MODIFY_REG(RCC->CFGR, RCC_CFGR_ADCPRE, RCC_CFGR_ADCPRE_DIV6);
    gpio.Pin = GPIO_PIN_0;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOC, &gpio);
    ADC1->CR1 = 0U; /* 独立、12 位、单次规则转换，不开 ADC 中断。 */
    ADC1->CR2 = ADC_CR2_EXTSEL; /* EXTSEL=111：规则组软件触发，右对齐。 */
    /* 239.5 个 ADC 周期约 20 us，给 5k 分压源和内部参考留足采样时间。 */
    ADC1->SMPR1 = ADC_SMPR1_SMP10 | ADC_SMPR1_SMP17;
    ADC1->SMPR2 = 0U;
    ADC1->SQR1 = 0U; /* L=0：一笔只转换一个通道。 */
    ADC1->SQR2 = 0U;
    ADC1->SR = 0U;
    l_calibrated = false;
    l_initialized = true;
    l_state = ADC_OFF;
    l_since = BSP_uptimeMs();
    return BSP_OK;
}

void BSP_adcProcess(void) {
    uint32_t now = BSP_uptimeMs();
    uint32_t elapsed = now - l_since; /* 无符号差兼容毫秒计数回绕。 */
    if (!l_initialized) { return; }
    switch (l_state) {
        case ADC_OFF:
            if (elapsed >= APP_CFG_ADC_STABILIZE_MS) {
                ADC1->CR2 |= ADC_CR2_ADON | ADC_CR2_TSVREFE;
                l_since = now;
                l_state = ADC_POWER;
            }
            break;
        case ADC_POWER:
            if (elapsed >= APP_CFG_ADC_STABILIZE_MS) {
                ADC1->CR2 |= ADC_CR2_RSTCAL;
                l_since = now;
                l_state = ADC_RESET_CAL;
            }
            break;
        case ADC_RESET_CAL:
        case ADC_CAL:
            if ((ADC1->CR2 & ((l_state == ADC_CAL) ? ADC_CR2_CAL : ADC_CR2_RSTCAL)) == 0U) {
                if (l_state == ADC_RESET_CAL) {
                    ADC1->CR2 |= ADC_CR2_CAL;
                    l_state = ADC_CAL;
                    l_since = now;
                } else {
                    l_calibrated = true;
                    l_state = ADC_IDLE;
                    BSP_adcCancel();
                }
            } else if (elapsed >= APP_CFG_ADC_CAL_TIMEOUT_MS) {
                ADC1->CR2 &= ~(ADC_CR2_ADON | ADC_CR2_TSVREFE);
                l_state = ADC_FAULT;
            }
            break;
        case ADC_WAKE:
            if (elapsed >= APP_CFG_ADC_STABILIZE_MS) {
                ADC1->CR2 |= ADC_CR2_EXTTRIG | ADC_CR2_SWSTART;
                l_since = now;
                l_state = ADC_CONVERT;
            }
            break;
        case ADC_CONVERT:
            if ((ADC1->SR & ADC_SR_EOC) != 0U) {
                l_raw = (uint16_t)ADC1->DR; /* 读 DR 同时清 EOC。 */
                finish(BSP_OK);
            } else if (elapsed >= APP_CFG_ADC_CONVERSION_TIMEOUT_MS) {
                finish(BSP_ERR_TIMEOUT);
            }
            break;
        default: break;
    }
}

bsp_status_t BSP_adcStart(BspAdcChannel channel) {
    if (channel != BSP_ADC_BATTERY && channel != BSP_ADC_VREFINT) { return BSP_ERR_PARAM; }
    if (!l_initialized || l_state == ADC_FAULT) { return BSP_ERR_IO; }
    if (l_state != ADC_IDLE) { return BSP_ERR_BUSY; }
    ADC1->SQR3 = (uint32_t)channel;
    ADC1->SR = 0U;
    l_since = BSP_uptimeMs();
    if ((ADC1->CR2 & ADC_CR2_ADON) == 0U) {
        ADC1->CR2 |= ADC_CR2_ADON | ADC_CR2_TSVREFE;
        l_state = ADC_WAKE; /* 同时等待 ADC 与 VREFINT 稳定，不阻塞调用者。 */
    } else {
        ADC1->CR2 |= ADC_CR2_EXTTRIG | ADC_CR2_SWSTART;
        l_state = ADC_CONVERT;
    }
    return BSP_OK;
}

bool BSP_adcResult(bsp_status_t *result, uint16_t *raw) {
    if (result == (bsp_status_t *)0 || raw == (uint16_t *)0 || l_state != ADC_DONE) { return false; }
    *result = l_result;
    if (l_result == BSP_OK) { *raw = l_raw; }
    l_state = ADC_IDLE;
    return true;
}

void BSP_adcCancel(void) {
    if (l_calibrated) {
        ADC1->CR2 &= ~(ADC_CR2_ADON | ADC_CR2_TSVREFE | ADC_CR2_EXTTRIG);
        ADC1->SR = 0U;
        l_state = ADC_IDLE;
    }
}
