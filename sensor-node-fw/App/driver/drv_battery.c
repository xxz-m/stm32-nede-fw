/*============================================================================
 * drv_battery.c —— Driver 层：分压稳定等待、多次 ADC 平均与电压换算
 * 目标硬件：STM32F103RCT6 + 单节锂电池；所属内核：QV，禁止忙等。
 * 器件时序与换算在本层，PC1/PC0 和 ADC 寄存器只由 BSP 操作。
 *===========================================================================*/
#include "drv_battery.h"
#include "bsp.h"
#include "app_config.h"

#if APP_CFG_BATTERY_AVG_COUNT < 1 || APP_CFG_BATTERY_AVG_COUNT > 16
#error "Battery averaging count must be 1..16"
#endif

typedef enum { BAT_IDLE, BAT_SETTLE, BAT_READ, BAT_DONE } BatteryState;
static struct {
    BatteryState state;
    bool initialized;
    bool pending;
    bool reference;
    uint8_t count;
    uint32_t since;
    uint32_t bat_sum;
    uint32_t ref_sum;
    drv_status_t result;
    DrvBatterySample sample;
    DrvBatteryDiagnostic diagnostic;
} l_bat;

/** @brief 清除上次诊断；忙请求不能抹掉待取作业的信息。 */
static void resetDiagnostic(void) {
    l_bat.diagnostic = (DrvBatteryDiagnostic){0};
    l_bat.diagnostic.reason = "none";
}

/** @brief 统一退出作业，成功、错误和超时都关分压支路与 ADC。 */
static void finish(drv_status_t result) {
    BSP_batteryGateSet(false);
    BSP_adcCancel();
    l_bat.pending = false;
    l_bat.result = result;
    l_bat.state = BAT_DONE;
}

/** @brief 标记具体失败原因，仍统一走开关与 ADC 清理路径。 */
static void fail(drv_status_t result, char const *reason) {
    l_bat.diagnostic.reason = reason;
    finish(result);
}

drv_status_t DRV_Battery_Init(void) {
    if (l_bat.initialized && l_bat.state != BAT_IDLE) { return DRV_ERR_BUSY; }
    l_bat.state = BAT_IDLE;
    l_bat.pending = false;
    resetDiagnostic();
    l_bat.initialized = (BSP_adcInit() == BSP_OK);
    return l_bat.initialized ? DRV_OK : DRV_ERR_BUS;
}

drv_status_t DRV_Battery_StartSample(void) {
    if (!l_bat.initialized) { return DRV_ERR_NODEV; }
    if (l_bat.state != BAT_IDLE) { return DRV_ERR_BUSY; }
    resetDiagnostic();
    l_bat.pending = false;
    l_bat.reference = false;
    l_bat.count = 0U;
    l_bat.bat_sum = 0U;
    l_bat.ref_sum = 0U;
    l_bat.since = BSP_uptimeMs();
    BSP_batteryGateSet(true);
    l_bat.state = BAT_SETTLE;
    return DRV_OK;
}

void DRV_Battery_Process(void) {
    bsp_status_t status;
    uint16_t raw;
    uint32_t elapsed;
    BSP_adcProcess(); /* 即便没有采样作业，也必须完成启动校准。 */
    if (l_bat.state == BAT_IDLE || l_bat.state == BAT_DONE) { return; }
    elapsed = BSP_uptimeMs() - l_bat.since;
    if (elapsed >= APP_CFG_BATTERY_JOB_TIMEOUT_MS) { fail(DRV_ERR_TIMEOUT, "job_timeout"); return; }
    if (l_bat.state == BAT_SETTLE) {
        if (elapsed < APP_CFG_BATTERY_SETTLE_MS) { return; }
        l_bat.state = BAT_READ;
    }
    if (!l_bat.pending) {
        l_bat.diagnostic.reference = l_bat.reference;
        status = BSP_adcStart(l_bat.reference ? BSP_ADC_VREFINT : BSP_ADC_BATTERY);
        l_bat.diagnostic.adc_status = (int)status;
        if (status == BSP_OK) { l_bat.pending = true; }
        else if (status != BSP_ERR_BUSY) { fail(DRV_ERR_BUS, "adc_start"); }
        return;
    }
    if (!BSP_adcResult(&status, &raw)) { return; }
    l_bat.pending = false;
    l_bat.diagnostic.adc_status = (int)status;
    if (status != BSP_OK) {
        fail(status == BSP_ERR_TIMEOUT ? DRV_ERR_TIMEOUT : DRV_ERR_BUS, "adc_result");
        return;
    }
    if (raw > APP_CFG_ADC_FULL_SCALE) {
        if (l_bat.reference) { l_bat.diagnostic.vref_raw = raw; }
        else { l_bat.diagnostic.raw = raw; }
        fail(DRV_ERR_BUS, "raw_range");
        return;
    }
    if (l_bat.reference) {
        l_bat.ref_sum += raw;
        l_bat.diagnostic.ref_count = (uint8_t)(l_bat.count + 1U);
        l_bat.diagnostic.vref_raw = (uint16_t)(l_bat.ref_sum / l_bat.diagnostic.ref_count);
    } else {
        l_bat.bat_sum += raw;
        l_bat.diagnostic.bat_count = (uint8_t)(l_bat.count + 1U);
        l_bat.diagnostic.raw = (uint16_t)(l_bat.bat_sum / l_bat.diagnostic.bat_count);
    }
    if (++l_bat.count < APP_CFG_BATTERY_AVG_COUNT) { return; }
    if (!l_bat.reference) {
        /* 电池通道读完就释放 PC1，后面的内部参考转换不需要分压电流。 */
        BSP_batteryGateSet(false);
        l_bat.reference = true;
        l_bat.count = 0U;
    } else {
        uint32_t vdda;
        uint32_t voltage;
        if (l_bat.ref_sum == 0U) { fail(DRV_ERR_BUS, "vref_zero"); return; }
        vdda = (APP_CFG_ADC_VREFINT_MV * APP_CFG_ADC_FULL_SCALE * APP_CFG_BATTERY_AVG_COUNT
                + l_bat.ref_sum / 2U) / l_bat.ref_sum;
        /* 先用原始累加值比值换算，避免对 VDDA 和平均值分别截断产生额外误差。
         * 10k/10k 的比例为 2；内部参考典型值不能代替万用表实测校准。 */
        voltage = (l_bat.bat_sum * APP_CFG_ADC_VREFINT_MV * APP_CFG_BATTERY_DIVIDER
                   + l_bat.ref_sum / 2U) / l_bat.ref_sum;
        l_bat.diagnostic.vdda_mV = vdda;
        l_bat.diagnostic.voltage_mV = voltage;
        if (vdda < APP_CFG_ADC_VDDA_MIN_MV || vdda > APP_CFG_ADC_VDDA_MAX_MV) {
            fail(DRV_ERR_BUS, "vdda_range"); return;
        }
        if (voltage > APP_CFG_BATTERY_MAX_MV) {
            fail(DRV_ERR_BUS, "battery_range"); return;
        }
        l_bat.sample.raw = (uint16_t)(l_bat.bat_sum / APP_CFG_BATTERY_AVG_COUNT);
        l_bat.sample.vref_raw = (uint16_t)(l_bat.ref_sum / APP_CFG_BATTERY_AVG_COUNT);
        l_bat.sample.vdda_mV = (uint16_t)vdda;
        l_bat.sample.voltage_mV = (uint16_t)voltage;
        finish(DRV_OK);
    }
}

bool DRV_Battery_IsDone(void) { return l_bat.state == BAT_DONE; }

DrvBatteryDiagnostic DRV_Battery_GetDiagnostic(void) {
    return l_bat.diagnostic;
}

drv_status_t DRV_Battery_GetSample(DrvBatterySample *sample) {
    drv_status_t result;
    if (sample == (DrvBatterySample *)0) { return DRV_ERR_PARAM; }
    if (l_bat.state != BAT_DONE) { return DRV_ERR_BUSY; }
    result = l_bat.result;
    if (result == DRV_OK) { *sample = l_bat.sample; }
    l_bat.state = BAT_IDLE;
    return result;
}

void DRV_Battery_Cancel(void) {
    BSP_batteryGateSet(false);
    BSP_adcCancel();
    l_bat.pending = false;
    l_bat.state = BAT_IDLE;
}
