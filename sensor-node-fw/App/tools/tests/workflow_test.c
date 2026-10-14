/* 主机验证：真实批采样、蓝牙窗口及 MQTT 状态机；硬件与时钟由测试替身注入。
 * 不属于固件编译源，不验证实际无线链路、电气时序或云端接收。 */
#include "bsp.h"
#include "sensor_cycle.h"
#include "ble_runtime.h"
#include "report_period.h"
#include "store.h"
#include "backend/ble_backend.h"
#include "drv_battery.h"
#include "svc_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 复用真实静态运行期状态机，避免仅为测试扩大组件公共 API。 */
#include "../../component/modem/modem_component.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
/* 只替换Store介质/调度边界，周期校验和BLE协议调用真实实现。 */
static char stored_period[8] = "20";
static svc_status_t store_status = SVC_OK;
static unsigned store_writes;
char const *App_Store_Value(uint8_t id) { CHECK(id == SVC_PARAM_TRANSINTVL); return stored_period; }
svc_status_t App_Store_SetName(char const *name, char const *value) {
    CHECK(strcmp(name,"transintvl")==0);
    if (store_status != SVC_OK) { return store_status; }
    CHECK(strlen(value)<sizeof(stored_period)); strcpy(stored_period,value); ++store_writes;
    return SVC_OK;
}
static uint32_t clock_ms;
static unsigned sensor_starts, battery_starts, sensor_ticks, battery_ticks;
static bool sensor_busy, sensor_done, sensor_stuck, battery_busy, battery_done;
static drv_status_t sensor_status, battery_status;
static int32_t angle;
uint32_t BSP_uptimeMs(void) { return clock_ms; }
void BSP_spiProcess(void) { }
void SVC_Log_Printf(svc_log_lvl_t lvl, char const *fmt, ...) { (void)lvl; (void)fmt; }
void DRV_Sensor_Process(void) {
    if (sensor_busy && !sensor_stuck && ++sensor_ticks >= 2U) { sensor_busy=false; sensor_done=true; }
}
bool DRV_Sensor_IsReady(void) { return !sensor_busy && !sensor_done; }
bool DRV_Sensor_IsDone(void) { return sensor_done; }
drv_status_t DRV_Sensor_StartSample(void) {
    if (!DRV_Sensor_IsReady()) { return DRV_ERR_BUSY; }
    sensor_busy=true; sensor_ticks=0U; ++sensor_starts; return DRV_OK;
}
drv_status_t DRV_Sensor_GetSample(DrvSensorSample *out) {
    unsigned i;
    if (!sensor_done) { return DRV_ERR_BUSY; }
    sensor_done=false;
    if (sensor_status != DRV_OK) { return sensor_status; }
    memset(out,0,sizeof(*out));
    for (i=0;i<DRV_SENSOR_AXIS_COUNT;++i) { out->angle_cdeg[i]=angle; }
    out->temp_cC=2530; angle+=100; return DRV_OK;
}
void DRV_Battery_Process(void) {
    if (battery_busy && ++battery_ticks >= 14U) { battery_busy=false; battery_done=true; }
}
bool DRV_Battery_IsDone(void) { return battery_done; }
drv_status_t DRV_Battery_StartSample(void) {
    CHECK(!battery_busy && !battery_done);
    battery_busy=true; battery_ticks=0U; ++battery_starts; return DRV_OK;
}
drv_status_t DRV_Battery_GetSample(DrvBatterySample *out) {
    CHECK(battery_done); battery_done=false;
    out->voltage_mV=3670U; return battery_status;
}
void DRV_Battery_Cancel(void) { battery_busy=false; battery_done=false; }
DrvBatteryDiagnostic DRV_Battery_GetDiagnostic(void) {
    DrvBatteryDiagnostic d={0}; d.reason="injected"; return d;
}
static void resetCycle(SensorCycle *c) {
    clock_ms=0; sensor_starts=battery_starts=0;
    sensor_busy=sensor_done=sensor_stuck=battery_busy=battery_done=false;
    sensor_status=battery_status=DRV_OK; angle=100;
    App_SensorCycle_Init(c);
}
static SensorCycleResult completeCycle(SensorCycle *c) {
    SensorCycleResult r;
    unsigned i;
    for(i=0;i<500U;++i) {
        clock_ms+=20U; App_SensorCycle_Process(c);
        if(App_SensorCycle_Take(c,&r)) { return r; }
    }
    CHECK(false); memset(&r,0,sizeof(r)); return r;
}
static void testCycle(void) {
    SensorCycle c; SensorCycleResult r; unsigned i;
    resetCycle(&c);
    for(i=0;i<500U;++i) { clock_ms+=20U; App_SensorCycle_Process(&c); }
    CHECK(sensor_starts==0U && battery_starts==0U);
    App_SensorCycle_Request(&c,SENSOR_TARGET_CLOUD); App_SensorCycle_Process(&c);
    App_SensorCycle_Request(&c,SENSOR_TARGET_BLE);
    for(i=0;i<10U;++i) { clock_ms+=20U; App_SensorCycle_Process(&c); }
    CHECK(!App_SensorCycle_Take(&c,&r)); /* 倾角已齐也必须等本次电池终态。 */
    r=completeCycle(&c);
    CHECK(r.valid && r.targets==3U && r.battery_valid && r.battery_mV==3670U);
    CHECK(sensor_starts==4U && battery_starts==1U && r.sensor.angle_cdeg[0]==250);
    angle=2000; App_SensorCycle_Request(&c,SENSOR_TARGET_CLOUD); r=completeCycle(&c);
    CHECK(r.sensor.angle_cdeg[0]==2150 && sensor_starts==8U); /* 不混入上一批。 */
    puts("PASS idle, merged requests, complete battery, and batch filter reset");
    resetCycle(&c); battery_status=DRV_ERR_BUS;
    App_SensorCycle_Request(&c,SENSOR_TARGET_CLOUD); r=completeCycle(&c);
    CHECK(r.valid && !r.battery_valid);
    resetCycle(&c); sensor_status=DRV_ERR_BUS;
    App_SensorCycle_Request(&c,SENSOR_TARGET_BLE); r=completeCycle(&c);
    CHECK(!r.valid && !battery_busy);
    resetCycle(&c); sensor_stuck=true;
    App_SensorCycle_Request(&c,SENSOR_TARGET_CLOUD); r=completeCycle(&c); CHECK(!r.valid);
    sensor_stuck=false; App_SensorCycle_Request(&c,SENSOR_TARGET_BLE);
    r=completeCycle(&c); CHECK(r.valid);
    resetCycle(&c); clock_ms=UINT32_MAX-50U;
    App_SensorCycle_Request(&c,SENSOR_TARGET_CLOUD); r=completeCycle(&c); CHECK(r.valid);
    puts("PASS invalid battery, sensor fault, timeout recovery, and time wrap");
}
static char input[256], last_command[MODEM_COMPONENT_COMMAND_MAX];
static size_t input_size, writes;
static unsigned port_status;
static size_t testReadPort(void *ctx,uint8_t *out,size_t cap) {
    size_t n=input_size<cap?input_size:cap; (void)ctx;
    memcpy(out,input,n); memmove(input,input+n,input_size-n); input_size-=n; return n;
}
static uint32_t tickPort(void *ctx) { (void)ctx; return clock_ms; }
static ble_port_status_t writeBle(void *ctx,uint8_t const *p,size_t n) {
    (void)ctx; CHECK(n<sizeof(last_command)); memcpy(last_command,p,n); last_command[n]=0;
    ++writes; return (ble_port_status_t)port_status;
}
static modem_port_status_t writeModem(void *ctx,uint8_t const *p,size_t n) {
    (void)ctx; CHECK(n<sizeof(last_command)); memcpy(last_command,p,n); last_command[n]=0;
    ++writes; return (modem_port_status_t)port_status;
}
static void feed(char const *line) { input_size=strlen(line); memcpy(input,line,input_size); }
static BlePort bp={NULL,writeBle,testReadPort,tickPort,NULL};
static BleConfig bc={"test-device","123456","test-device","CDAB0000-1234-5678-1234-56789ABCDE00","test","test","test"};
static void readyBle(BleComponent *b,BleRuntime *r) {
    clock_ms=0; writes=0; input_size=0; port_status=0;
    App_BleRuntime_Init(r); BLE_Component_Init(b,&bp,&bc,r->params,7U); b->state=BLE_COMPONENT_READY;
}
static void testBle(void) {
    BleComponent b; BleRuntime r; char buffer[200]; char const *sent; unsigned i;
    readyBle(&b,&r);
    CHECK(strcmp(BLE_Backend_Command(&bc,4,buffer,sizeof(buffer)),"AT+ADE=0\r\n")==0);
    CHECK(!App_BleRuntime_Process(&r,&b,0)); BLE_Component_Process(&b); CHECK(writes==0U);
    App_BleRuntime_OpenWindow(&r,0); (void)App_BleRuntime_Process(&r,&b,0); BLE_Component_Process(&b);
    CHECK(strcmp(last_command,"AT+ADE=1\r\n")==0 && b.advert_pending);
    feed("+OK\r\n"); BLE_Component_Process(&b); CHECK(b.advert_enabled);
    clock_ms=119000; App_BleRuntime_OpenWindow(&r,clock_ms);
    clock_ms=120001; CHECK(!App_BleRuntime_Process(&r,&b,clock_ms) && r.window_open);
    b.connected=b.authenticated=true;
    CHECK(App_BleRuntime_Process(&r,&b,clock_ms));
    CHECK(!App_BleRuntime_Process(&r,&b,clock_ms));
    clock_ms=239000; (void)App_BleRuntime_Process(&r,&b,clock_ms); BLE_Component_Process(&b);
    CHECK(!r.window_open && b.connected && !b.advert_pending);
    feed("+DISCONNECT-0\r\n"); BLE_Component_Process(&b);
    CHECK(!b.connected && b.advert_pending && strcmp(last_command,"AT+ADE=0\r\n")==0);
    feed("+OK\r\n"); BLE_Component_Process(&b); CHECK(!b.advert_enabled);
    puts("PASS default disabled, window restart/expiry, connected session, and disconnect disable");
    readyBle(&b,&r); b.connected=b.authenticated=true;
    CHECK(App_BleRuntime_Process(&r,&b,0));
    App_BleRuntime_Measurement(&r,2509,137,25,8859,3670,true,true);
    CHECK(!App_BleRuntime_Process(&r,&b,100));
    CHECK(BLE_Component_TakeSentReport(&b)==NULL); BLE_Component_Process(&b);
    sent=BLE_Component_TakeSentReport(&b); CHECK(sent && strstr(sent,"\"voltage\":3670"));
    CHECK(strstr(sent,"\"temperature\":25.09") && BLE_Component_TakeSentReport(&b)==NULL);
    r.params[6].value.value.integer=2;
    CHECK(!App_BleRuntime_Process(&r,&b,2000));
    CHECK(!App_BleRuntime_Process(&r,&b,119999));
    CHECK(!App_BleRuntime_Process(&r,&b,120000)); /* BLE不再启动独立周期。 */
    App_BleRuntime_Measurement(&r,2500,1,2,3,0,false,true);
    (void)App_BleRuntime_Process(&r,&b,5100); BLE_Component_Process(&b);
    sent=BLE_Component_TakeSentReport(&b); CHECK(sent && !strstr(sent,"\"voltage\""));
    App_BleRuntime_Measurement(&r,0,0,0,0,0,false,false);
    CHECK(!r.data_pending);
    puts("PASS fresh first sample, no duplicate BLE timer, real battery, invalid omission, and sent notification");
    readyBle(&b,&r); BLE_Component_SetAdvertising(&b,true);
    for(i=0;i<APP_CFG_BLE_ADVERT_ATTEMPTS;++i) {
        BLE_Component_Process(&b); CHECK(b.advert_pending);
        feed("+ERP\r\n"); BLE_Component_Process(&b);
        clock_ms+=APP_CFG_BLE_ADVERT_RETRY_MS;
    }
    CHECK(b.state==BLE_COMPONENT_FAILED);
    readyBle(&b,&r); port_status=BLE_PORT_ERROR; BLE_Component_SetAdvertising(&b,true);
    for(i=0;i<APP_CFG_BLE_ADVERT_ATTEMPTS;++i) { BLE_Component_Process(&b); clock_ms+=APP_CFG_BLE_ADVERT_RETRY_MS; }
    CHECK(b.state==BLE_COMPONENT_FAILED);
    readyBle(&b,&r); BLE_Component_SetAdvertising(&b,true);
    for(i=0;i<APP_CFG_BLE_ADVERT_ATTEMPTS;++i) {
        BLE_Component_Process(&b); CHECK(b.advert_pending);
        clock_ms+=BLE_COMPONENT_TIMEOUT_MS; BLE_Component_Process(&b);
        clock_ms+=APP_CFG_BLE_ADVERT_RETRY_MS;
    }
    CHECK(b.state==BLE_COMPONENT_FAILED && b.failure==BLE_FAILURE_TIMEOUT);
    readyBle(&b,&r); b.tx_head=2; b.tx_tail=1; b.tx_count=1; b.data_sent=true;
    BLE_Component_Start(&b); CHECK(b.tx_head==0 && b.tx_tail==0 && b.tx_count==0 && !b.data_sent);
    puts("PASS bounded AT response/write failures and restart queue reset");
}
extern void BLE_Protocol_Payload(BleComponent *,uint8_t const *,uint16_t);
static void setBlePeriod(BleComponent *b,char const *raw,bool ok) {
    char json[128];
    (void)snprintf(json,sizeof(json),"{\"cmd\":\"set\",\"param\":\"sample_interval\",\"v\":%s}",raw);
    b->tx_count=b->tx_head=b->tx_tail=0;
    BLE_Protocol_Payload(b,(uint8_t const *)json,(uint16_t)strlen(json));
    CHECK(b->tx_count==1U);
    CHECK(strstr(b->tx_queue[0],ok ? "\"ok\":true" : "\"ok\":false"));
}
static void testUnifiedPeriod(void) {
    BleComponent b; BleRuntime r;
    readyBle(&b,&r); b.connected=b.authenticated=true;
    CHECK(strcmp(r.params[6].unit,"min")==0 && r.params[6].minimum==1 && r.params[6].maximum==20);
    store_writes=0; store_status=SVC_OK; strcpy(stored_period,"20");
    setBlePeriod(&b,"2",true);
    CHECK(store_writes==1U && strcmp(stored_period,"2")==0);
    CHECK(App_ReportPeriod_Minutes()==2U && App_ReportPeriod_Milliseconds()==120000U);
    CHECK(r.params[6].value.value.integer==2);
    setBlePeriod(&b,"0",false); setBlePeriod(&b,"21",false);
    setBlePeriod(&b,"2.5",false); setBlePeriod(&b,"2abc",false);
    CHECK(store_writes==1U && App_ReportPeriod_Minutes()==2U);
    store_status=SVC_ERR_STATE; setBlePeriod(&b,"3",false);
    CHECK(r.params[6].value.value.integer==2 && App_ReportPeriod_Minutes()==2U);
    store_status=SVC_OK;
    App_BleRuntime_Init(&r); /* 模拟应用重建，持久参数仍来自Store。 */
    CHECK(App_ReportPeriod_Minutes()==2U);
    CHECK(App_ReportPeriod_SetMinutes(5U)==SVC_OK && App_ReportPeriod_Milliseconds()==300000U);
    strcpy(stored_period,"bad"); CHECK(App_ReportPeriod_Minutes()==APP_CFG_MODEM_TRANS_INTERVAL_DEFAULT);
    strcpy(stored_period,"1440"); CHECK(App_ReportPeriod_Minutes()==APP_CFG_MODEM_TRANS_INTERVAL_DEFAULT);
    strcpy(stored_period,"2");
    puts("PASS BLE protocol changes unified persisted minute period; bounds, malformed values, and store failure");
}
static char report_diag[MODEM_DIAG_LINE_MAX];
static unsigned report_diags;
/** @brief 捕获发布错误诊断，确认只保留错误文本、没有命令或测量负载。 */
static void captureModemTrace(void *ctx, char const *text) {
    (void)ctx;
    if (strstr(text,"runtime publish fail") != NULL) {
        CHECK(strlen(text)<sizeof(report_diag));
        strcpy(report_diag,text); ++report_diags;
        CHECK(strstr(text,"AT+MQTTPUB")==NULL && strstr(text,"temperature")==NULL);
    }
}
static ModemPort mp={NULL,writeModem,testReadPort,tickPort,captureModemTrace};
static void testModem(void) {
    ModemComponent m; ModemConfig cfg={0}; char saved[MODEM_COMPONENT_PAYLOAD_MAX];
    char command[MODEM_COMPONENT_COMMAND_MAX]; char const *result; bool success;
    clock_ms=0; port_status=0; input_size=writes=0; report_diags=0;
    cfg.product_id="test-product"; cfg.device_id="test-device";
    Modem_Component_Init(&m,&mp,&cfg); buildTopics(&m); finishPlan(&m);
    CHECK(!m.report_requested); m.cell_requested=false;
    m.data.temperature_cC=2509; m.data.battery_valid=true; m.data.battery_mV=3670;
    Modem_Component_RequestReport(&m); Modem_Component_Process(&m);
    CHECK(m.runtime_op==MODEM_RUNTIME_SIGNAL && strstr(last_command,"AT+CSQ"));
    feed("+CSQ: 15,99\r\nOK\r\n"); Modem_Component_Process(&m);
    CHECK(m.signal_revision==1U && m.data.signal_quality==15);
    Modem_Component_Process(&m); CHECK(m.runtime_op==MODEM_RUNTIME_REPORT);
    strcpy(saved,m.report_payload); strcpy(command,last_command);
    CHECK(strstr(saved,"\"signal_quality\":15") && strstr(saved,"\"battery_voltage\":3.670"));
    CHECK(Modem_Component_TakeReportResult(&m,&success)==NULL);
    m.data.temperature_cC=9900; strcpy(m.payload,"downlink-overwrite");
    feed("+CME ERROR:8002\r\n"); Modem_Component_Process(&m);
    result=Modem_Component_TakeReportResult(&m,&success);
    CHECK(result && !success && strcmp(result,saved)==0 && m.report_requested);
    CHECK(report_diags==1 && strstr(report_diag,"+CME ERROR:8002"));
    CHECK(strstr(report_diag,"reason=3") && strstr(report_diag,"attempt=1/4"));
    /* 8002 属于「会话风险」（2026-10-14 新机制）：下一次推进先复核会话状态，而不是直接重发。
     * 状态 5 = 会话仍可用 → 保留链路，后面的重发/信号刷新照旧。 */
    CHECK(m.session_check);
    Modem_Component_Process(&m);
    CHECK(m.runtime_op==MODEM_RUNTIME_STATE && strstr(last_command,"AT+MQTTSTAT?"));
    feed("+MQTTSTAT:5\r\nOK\r\n"); Modem_Component_Process(&m);
    CHECK(m.state==MODEM_COMPONENT_READY && !m.session_check);
    Modem_Component_RequestSignal(&m);
    clock_ms+=MODEM_COMPONENT_UPLINK_RETRY_MS; Modem_Component_Process(&m);
    CHECK(m.runtime_op==MODEM_RUNTIME_SIGNAL);
    feed("+CSQ: 18,99\r\nOK\r\n"); Modem_Component_Process(&m);
    CHECK(m.uplink_retry==1U); /* CSQ成功不能让发布无限重试。 */
    Modem_Component_Process(&m);
    CHECK(strcmp(command,last_command)==0 && strcmp(saved,m.report_payload)==0);
    feed("OK\r\n"); Modem_Component_Process(&m);
    result=Modem_Component_TakeReportResult(&m,&success);
    CHECK(result && success && strcmp(result,saved)==0 && !m.report_requested);
    CHECK(Modem_Component_TakeReportResult(&m,&success)==NULL);
    puts("PASS fresh first-report gate, CSQ refresh, frozen retry payload, and ACK-only success");
    Modem_Component_RequestReport(&m); Modem_Component_Process(&m);
    CHECK(m.runtime_op==MODEM_RUNTIME_SIGNAL);
    feed("ERROR\r\n"); Modem_Component_Process(&m); CHECK(m.data.signal_quality==99);
    Modem_Component_Process(&m); CHECK(strstr(m.report_payload,"\"signal_quality\":99"));
    CHECK(strstr(m.report_payload,"\"temperature\":99.00"));
    puts("PASS failed CSQ becomes unknown and next report uses new measurement");
    strcpy(saved,m.report_payload);
    { unsigned i;
      for(i=0;i<4;++i) {
        char attempt[32];
        if(i!=0U) {
            clock_ms+=MODEM_COMPONENT_UPLINK_RETRY_MS; Modem_Component_Process(&m);
            CHECK(m.runtime_op==MODEM_RUNTIME_REPORT && strcmp(saved,m.report_payload)==0);
        }
        feed(i==0U?"ERROR\r\n":i==1U?"+CMS ERROR:500\r\n":i==2U?"+MQTTPUB:FAIL\r\n":"+CME ERROR:8002\r\n");
        Modem_Component_Process(&m);
        sprintf(attempt,"attempt=%u/4",i+1U); CHECK(strstr(report_diag,attempt));
        CHECK(strstr(report_diag,i==0U?"ERROR":i==1U?"+CMS ERROR:500":i==2U?":FAIL":"+CME ERROR:8002"));
        CHECK(m.state==(i==3U?MODEM_COMPONENT_FAILED:MODEM_COMPONENT_READY));
        result=Modem_Component_TakeReportResult(&m,&success); CHECK(result && !success && strcmp(result,saved)==0);
      }
    }
    CHECK(report_diags==5U);
    puts("PASS raw CME/CMS/ERROR/FAIL diagnostics, frozen four attempts and terminal reconnect state");
}
/** @brief 在线期会话健康通知（2026-10-14 新增）：LOST 直接重建、RISK 先复核状态。
 *  对应现场：`+MQTTTO:5`（ping 超时）后云平台显示掉线，而设备侧上行还能发、下行静默。 */
static void testSessionCheck(void) {
    ModemComponent m; ModemConfig cfg={0};
    cfg.product_id="test-product"; cfg.device_id="test-device";
    /* ① `+MQTTREC:n` = 模组自己在重连 → 会话已丢，直接判链路失效（不走状态查询） */
    clock_ms=0; port_status=0; input_size=writes=0;
    Modem_Component_Init(&m,&mp,&cfg); buildTopics(&m); finishPlan(&m);
    feed("+MQTTREC:1\r\n"); Modem_Component_Process(&m);
    CHECK(m.state==MODEM_COMPONENT_FAILED && !m.session_check);
    /* ② `+MQTTTO:5` = ping 超时 → 先复核：只有模组报「已连接（5）」才算会话可用 */
    Modem_Component_Init(&m,&mp,&cfg); buildTopics(&m); finishPlan(&m);
    feed("+MQTTTO:5\r\n"); Modem_Component_Process(&m);
    CHECK(m.session_check && m.runtime_op==MODEM_RUNTIME_STATE);
    CHECK(strstr(last_command,"AT+MQTTSTAT?")!=NULL);
    feed("+MQTTSTAT:5\r\nOK\r\n"); Modem_Component_Process(&m);
    CHECK(m.state==MODEM_COMPONENT_READY && !m.session_check);
    /* ③ 在线期收到 `+CME ERROR:8002` 同样触发复核；状态不是 5 → 判会话不可用 */
    feed("+CME ERROR:8002\r\n"); Modem_Component_Process(&m);
    CHECK(m.session_check && m.runtime_op==MODEM_RUNTIME_STATE);
    feed("+MQTTSTAT:3\r\nOK\r\n"); Modem_Component_Process(&m);
    CHECK(m.state==MODEM_COMPONENT_FAILED && !m.session_check);
    puts("PASS online session health: LOST rebuilds, RISK re-checks AT+MQTTSTAT and rebuilds when not 5");
}
/** @brief 订阅集合：`command` 必须在；开了「只订 command」开关时其余下行主题不分配订阅序号。
 *  对应 2026-10-14 的用户决定（现场订到第 2 条就被 broker 掐断，见改动日志 `#138`）。 */
static void testSubscribeSet(void) {
    ModemComponent m; ModemConfig cfg={0};
    unsigned idx, subscribed = 0U;
    cfg.product_id="test-product"; cfg.device_id="test-device";
    Modem_Component_Init(&m,&mp,&cfg); buildTopics(&m);
    for (idx=0U; idx<(unsigned)MODEM_TOPIC_COUNT; ++idx) {
        if (m.topic[idx].subscribe_id != 0U) {
            ++subscribed;
#if (MODEM_COMPONENT_TOPIC_SUBSCRIBE_COMMAND_ONLY != 0U)
            CHECK(idx==(unsigned)MODEM_TOPIC_COMMAND);
#endif
        }
    }
    CHECK(subscribed>=1U);
    CHECK(m.topic[MODEM_TOPIC_COMMAND].registered && m.topic[MODEM_TOPIC_COMMAND].subscribe_id!=0U);
    CHECK(strstr(m.topic[MODEM_TOPIC_COMMAND].name,"/command")!=NULL);
#if (MODEM_COMPONENT_TOPIC_SUBSCRIBE_COMMAND_ONLY != 0U)
    CHECK(subscribed==1U);
    puts("PASS subscribe set reduced to the command topic only (others keep subscribe_id 0)");
#else
    puts("PASS command topic is subscribed (command-only switch is off)");
#endif
}
int main(void) { testCycle(); testBle(); testUnifiedPeriod(); testModem(); testSessionCheck(); testSubscribeSet(); puts("workflow tests: 11 groups passed"); return 0; }
