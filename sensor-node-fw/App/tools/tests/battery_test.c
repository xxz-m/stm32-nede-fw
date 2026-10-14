/*============================================================================
 * 主机验证：编译实际电池 Driver 与 4G 组包器，注入 ADC 忙、超时与非法原始值。
 * 无硬件，不验证 PC1 耐压、电气时序与测量精度；不属于固件编译源。
 *===========================================================================*/
#include "bsp.h"
#include "drv_battery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 将真实静态组包器纳入同一翻译单元，避免为测试扩大组件公开 API。 */
#include "../../component/modem/modem_component.c"

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static uint32_t now;
static bool gate, pending, stuck, busy, start_fault;
static bsp_status_t injected;
static uint16_t bat_raw, ref_raw;
static BspAdcChannel channel;
static unsigned starts, conversions;

uint32_t BSP_uptimeMs(void) { return now; }
void BSP_batteryGateSet(bool enabled) { gate = enabled; }
bsp_status_t BSP_adcInit(void) { pending = false; gate = false; return BSP_OK; }
void BSP_adcProcess(void) { }
void BSP_adcCancel(void) { pending = false; }
bsp_status_t BSP_adcStart(BspAdcChannel ch) {
    if (start_fault) { return BSP_ERR_IO; }
    if (busy || pending) { return BSP_ERR_BUSY; }
    channel = ch;
    pending = true;
    ++starts;
    return BSP_OK;
}
bool BSP_adcResult(bsp_status_t *result, uint16_t *raw) {
    if (!pending || stuck) { return false; }
    CHECK(channel != BSP_ADC_BATTERY || gate);
    CHECK(channel != BSP_ADC_VREFINT || !gate);
    pending = false;
    *result = injected;
    if (injected == BSP_OK) { *raw = (channel == BSP_ADC_BATTERY) ? bat_raw : ref_raw; }
    ++conversions;
    return true;
}

/** @brief 每个用例重新初始化；原始值模拟 3.3 V 参考下的 4.2 V 电池。 */
static void reset(void) {
    DRV_Battery_Cancel();
    now = 0U;
    starts = conversions = 0U;
    stuck = busy = start_fault = false;
    injected = BSP_OK;
    bat_raw = 2606U;
    ref_raw = 1489U;
    CHECK(DRV_Battery_Init() == DRV_OK);
}
static void complete(void) {
    unsigned i;
    for (i = 0U; i < 100U && !DRV_Battery_IsDone(); ++i) {
        now += APP_CFG_SCL3300_PROCESS_PERIOD_MS;
        DRV_Battery_Process();
    }
    CHECK(DRV_Battery_IsDone());
    CHECK(!gate && !pending);
}
static void expectFailure(drv_status_t expected, char const *reason) {
    DrvBatterySample sample = {99U, 99U, 99U, 99U};
    DrvBatteryDiagnostic diagnostic;
    CHECK(DRV_Battery_StartSample() == DRV_OK);
    complete();
    CHECK(DRV_Battery_GetSample(&sample) == expected);
    diagnostic = DRV_Battery_GetDiagnostic();
    CHECK(strcmp(diagnostic.reason, reason) == 0); /* 消费错误后仍能读取原因。 */
    CHECK(sample.voltage_mV == 99U && !DRV_Battery_IsDone());
    CHECK(DRV_Battery_GetSample(&sample) == DRV_ERR_BUSY);
}

int main(void) {
    DrvBatterySample sample;
    DrvBatteryDiagnostic diagnostic;
    ModemComponent modem;
    ModemConfig config;
    reset();
    CHECK(DRV_Battery_StartSample() == DRV_OK && gate);
    CHECK(DRV_Battery_StartSample() == DRV_ERR_BUSY);
    CHECK(DRV_Battery_Init() == DRV_ERR_BUSY);
    now = APP_CFG_BATTERY_SETTLE_MS - 1U;
    DRV_Battery_Process();
    CHECK(starts == 0U); /* RC 等待期间不启动 ADC。 */
    complete();
    CHECK(conversions == 2U * APP_CFG_BATTERY_AVG_COUNT);
    CHECK(DRV_Battery_StartSample() == DRV_ERR_BUSY); /* 待取结果不能被覆盖。 */
    CHECK(DRV_Battery_GetSample(NULL) == DRV_ERR_PARAM && DRV_Battery_IsDone());
    CHECK(DRV_Battery_GetSample(&sample) == DRV_OK);
    CHECK(sample.raw == 2606U && sample.vref_raw == 1489U);
    CHECK(sample.vdda_mV == 3300U && sample.voltage_mV == 4200U);
    puts("PASS normal/settling/busy/result consumption");

    reset(); ref_raw = 1638U; bat_raw = 2867U;
    CHECK(DRV_Battery_StartSample() == DRV_OK); complete();
    CHECK(DRV_Battery_GetSample(&sample) == DRV_OK);
    CHECK(sample.vdda_mV == 3000U && sample.voltage_mV == 4201U);
    puts("PASS VDDA variation compensation");

    reset(); injected = BSP_ERR_TIMEOUT; expectFailure(DRV_ERR_TIMEOUT, "adc_result");
    reset(); injected = BSP_ERR_IO; expectFailure(DRV_ERR_BUS, "adc_result");
    diagnostic = DRV_Battery_GetDiagnostic();
    CHECK(diagnostic.adc_status == BSP_ERR_IO && diagnostic.bat_count == 0U);
    reset(); start_fault = true; expectFailure(DRV_ERR_BUS, "adc_start");
    reset(); ref_raw = 0U; expectFailure(DRV_ERR_BUS, "vref_zero");
    diagnostic = DRV_Battery_GetDiagnostic();
    CHECK(diagnostic.raw == 2606U && diagnostic.vref_raw == 0U);
    CHECK(diagnostic.bat_count == 4U && diagnostic.ref_count == 4U && diagnostic.reference);
    reset(); ref_raw = 50U; expectFailure(DRV_ERR_BUS, "vdda_range");
    diagnostic = DRV_Battery_GetDiagnostic();
    CHECK(diagnostic.vdda_mV == 98280U && diagnostic.voltage_mV == 125088U);
    reset(); bat_raw = 4095U; expectFailure(DRV_ERR_BUS, "battery_range");
    reset(); bat_raw = 5000U; expectFailure(DRV_ERR_BUS, "raw_range");
    CHECK(DRV_Battery_GetDiagnostic().raw == 5000U);
    reset(); ref_raw = 5000U; expectFailure(DRV_ERR_BUS, "raw_range");
    CHECK(DRV_Battery_GetDiagnostic().vref_raw == 5000U);
    /* 失败后的新作业不能沿用上次原始值或原因。 */
    CHECK(DRV_Battery_StartSample() == DRV_OK);
    diagnostic = DRV_Battery_GetDiagnostic();
    CHECK(strcmp(diagnostic.reason, "none") == 0 && diagnostic.raw == 0U && diagnostic.ref_count == 0U);
    DRV_Battery_Cancel();
    puts("PASS ADC timeout/IO/zero VREF/range errors and gate release");

    reset(); stuck = true; expectFailure(DRV_ERR_TIMEOUT, "job_timeout");
    reset(); busy = true; expectFailure(DRV_ERR_TIMEOUT, "job_timeout");
    reset(); now = UINT32_MAX - 10U;
    CHECK(DRV_Battery_StartSample() == DRV_OK); complete();
    CHECK(DRV_Battery_GetSample(&sample) == DRV_OK);
    reset(); CHECK(DRV_Battery_StartSample() == DRV_OK);
    DRV_Battery_Cancel(); CHECK(!gate && !pending && !DRV_Battery_IsDone());
    CHECK(DRV_Battery_StartSample() == DRV_OK); complete();
    CHECK(DRV_Battery_GetSample(&sample) == DRV_OK);
    puts("PASS job timeout/time wrap/cancel/restart");

    (void)memset(&modem, 0, sizeof(modem));
    (void)memset(&config, 0, sizeof(config));
    config.device_id = "battery-test"; modem.config = &config;
    modem.data.battery_mV = 4201U;
    CHECK(buildReport(&modem));
    CHECK(strstr(modem.payload, "battery_voltage") == NULL);
    modem.data.battery_valid = true;
    CHECK(buildReport(&modem));
    CHECK(strstr(modem.payload, "\"battery_voltage\":4.201") != NULL);
    CHECK(strstr(modem.payload, "{,") == NULL);
    modem.data.battery_valid = false;
    CHECK(buildReport(&modem));
    CHECK(strstr(modem.payload, "battery_voltage") == NULL);
    puts("PASS real MQTT JSON: volts/precision/invalid field omitted");
    puts("battery tests: 5 groups passed");
    return 0;
}
