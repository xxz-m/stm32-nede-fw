/*============================================================================
 * 主机验证：实际按键 Driver 与 App 上报接线，替换 GPIO/时钟，不访问硬件。
 * 验证消抖、长按、上电按住、回绕、离线待发与在途上报；不代表云端收包验证。
 *===========================================================================*/
#include "bsp.h"
#include "modem_report.h"
#include "drv_button.h"
#include "svc_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static uint32_t now;
static bool pressed;
static unsigned init_count;

uint32_t BSP_uptimeMs(void) { return now; }
void BSP_buttonInit(void) { ++init_count; }
bool BSP_buttonIsPressed(void) { return pressed; }
void SVC_Log_Printf(svc_log_lvl_t lvl, char const *fmt, ...) { (void)lvl; (void)fmt; }

static bool poll(DrvButton *button, uint32_t time, bool level) {
    now = time;
    pressed = level;
    return DRV_Button_Poll(button);
}


int main(void) {
    DrvButton key;
    ModemReportRequest button;
    ModemComponent component;
    now = 0U; pressed = false;
    DRV_Button_Init(&key);
    CHECK(init_count == 1U);
    {
        uint32_t d = APP_CFG_BUTTON_DEBOUNCE_MS;
        CHECK(d > 1U);
        CHECK(!poll(&key, 10U, true));
        CHECK(!poll(&key, 11U, false));
        CHECK(!poll(&key, 12U, true));
        CHECK(!poll(&key, 12U+d-1U, true));
        CHECK(poll(&key, 12U+d, true));
        CHECK(!poll(&key, 2000U, true)); /* 长按不重复。 */
        CHECK(!poll(&key, 2010U, false));
        CHECK(!poll(&key, 2011U, true)); /* 未稳定松开，不能再次触发。 */
        CHECK(!poll(&key, 2100U, true));
        CHECK(!poll(&key, 2110U, false));
        CHECK(!poll(&key, 2110U+d, false));
        CHECK(!poll(&key, 2200U, true));
        CHECK(poll(&key, 2200U+d, true));
        puts("PASS press/release bounce, hold, and second press");
        now=0U; pressed=true; DRV_Button_Init(&key);
        CHECK(!poll(&key,1000U,true));
        CHECK(!poll(&key,1010U,false));
        CHECK(!poll(&key,1010U+d,false));
        CHECK(!poll(&key,1100U,true));
        CHECK(poll(&key,1100U+d,true));
        now=UINT32_MAX-20U; pressed=false; DRV_Button_Init(&key);
        CHECK(!poll(&key,UINT32_MAX-10U,true));
        CHECK(poll(&key,(uint32_t)(UINT32_MAX-10U+d),true));
    }
    DRV_Button_Init(NULL); CHECK(!DRV_Button_Poll(NULL));
    puts("PASS startup held, millisecond wrap, and null input");

    (void)memset(&component, 0, sizeof(component));
    now = 0U; pressed = false; App_ModemReport_Init(&button);
    App_ModemReport_Request(&button); CHECK(button.pending);
    App_ModemReport_TrySend(&button, &component, true);
    CHECK(button.pending && !component.report_requested); /* 离线保留。 */
    component.state = MODEM_COMPONENT_READY;
    component.report_enabled = true;
    App_ModemReport_TrySend(&button, &component, false);
    CHECK(button.pending && !component.report_requested); /* 没有采样不能发全零。 */
    App_ModemReport_TrySend(&button, &component, true);
    CHECK(!button.pending && component.report_requested);
    component.report_requested = false;
    App_ModemReport_TrySend(&button, &component, true);
    CHECK(!component.report_requested); /* 已交付的请求不能再次发送。 */
    puts("PASS offline pending, first measurement, and one-shot request");

    /* 新按下遇到上一帧在途时不能把请求交给会清位的旧作业。 */
    pressed = false; App_ModemReport_Init(&button); App_ModemReport_Request(&button);
    component.runtime_op = MODEM_RUNTIME_REPORT;
    component.report_requested = true;
    App_ModemReport_TrySend(&button, &component, true);
    CHECK(button.pending);
    component.report_requested = false;
    App_ModemReport_TrySend(&button, &component, true);
    CHECK(button.pending); /* 即使请求位已清，也要等旧上报离开在途态。 */
    component.runtime_op = MODEM_RUNTIME_NONE;
    component.report_requested = true; /* 也不能覆盖已经排队但尚未开始的报告。 */
    App_ModemReport_TrySend(&button, &component, true);
    CHECK(button.pending);
    component.report_requested = false;
    component.report_enabled = false;
    App_ModemReport_TrySend(&button, &component, true);
    CHECK(button.pending && !component.report_requested);
    component.report_enabled = true;
    App_ModemReport_TrySend(&button, &component, true);
    CHECK(!button.pending && component.report_requested);
    puts("PASS report in flight and report_enabled gate");
    puts("button tests: 4 groups passed");
    return 0;
}
