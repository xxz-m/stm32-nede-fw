/* 主机测试：真实SCL3300驱动与批采样，SPI响应用滞后一帧的器件模型注入；不代替L5。 */
#include "bsp.h"
#include "sensor_cycle.h"
#include "drv_battery.h"
#include "svc_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* 直接验证真实私有换算及状态机，避免为测试增加固件公共接口。 */
#include "../../driver/drv_scl3300.c"
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
static uint32_t clock_ms, sample_index;
static unsigned previous_address;
static bool spi_pending, battery_done;
static uint16_t y_raw[4];
/* 器件状态摘要模型（手册 5.1.5 / 6.3 / 6.3.1，按 2026-10-14 上板实测行为建）：
 *   - 摘要里只要有**任何**一位，所有应答就带 RS=11（用户实测就是这样把第一个数据帧
 *     判成错误的）；
 *   - 读一次摘要即清零，但**返回的是清除前的值**，所以驱动器必须读到自己看到 0 为止；
 *   - 写模式/复位会重新置位 PWR(bit4)+MODE_CHANGE(bit1)（实测值 0x12）；
 *   - `arm_on_first_data` 模拟「初始化之后器件又自己把这两位置起来」，用来复现
 *     「RS=11 → 整批采样作废」的用户现象。 */
static uint16_t summary;
static bool arm_on_first_data, arm_done;
static unsigned armed_count;
uint32_t BSP_uptimeMs(void) { return clock_ms; }
void BSP_spiProcess(void) { }
bool BSP_spiIsBusy(void) { return spi_pending; }
/* SCL3300 追踪输出在主机侧丢弃：本测试验证状态机与换算，不验证串口打印。
 * 三者必须定义，否则 APP_CFG_SCL3300_TRACE=1U 时 -Werror 报隐式声明。 */
void BSP_print(char const *str) { (void)str; }
void BSP_printU32(uint32_t value) { (void)value; }
void BSP_printLine(char const *str) { (void)str; }
/** @brief 参考CRC独立于驱动函数，按24位MSB先行、初值FF、多项式1D、最终取反。 */
static uint8_t responseCrc(uint8_t const *bytes) {
    unsigned crc=255U,i,b;
    for(i=0;i<3U;++i) {
        crc^=bytes[i];
        for(b=0;b<8U;++b) { crc=((crc<<1U)^((crc&128U)?29U:0U))&255U; }
    }
    return (uint8_t)(crc^255U);
}
/** @brief 根据上一条请求返回响应，首帧旧数据不得被误用为当前量。 */
bsp_status_t BSP_spiTransferStart(BspSpiDevice device, uint8_t const *tx, uint8_t *rx, uint16_t length) {
    unsigned address=(tx[0]>>2U)&31U; uint16_t data=0;
    CHECK(device==BSP_SPI_SENSOR && length==4 && !spi_pending);
    /* ① 先生成「上一条命令」的应答（错帧协议），此时本帧新命令还没生效。 */
    /* NOTE (STATUS 读不另起一次采样)：重做本点时驱动会在命令帧前插一条 STATUS 读，
     * 那不是新的采样点，不能让它把 Y 轴样本序号往前推。 */
    if((address==SCL_ADDR_ANG_X) && (previous_address!=SCL_ADDR_ANG_X) &&
       (previous_address!=SCL_ADDR_STATUS)) { ++sample_index; }
    switch(previous_address) {
        case SCL_ADDR_ANG_X: data=3976; break;
        case SCL_ADDR_ANG_Y: data=y_raw[sample_index%4U]; break;
        case SCL_ADDR_ANG_Z: data=16384; break;
        case SCL_ADDR_ACC_X: data=12000; break;
        case SCL_ADDR_TEMP: data=5634; break;
        case SCL_ADDR_WHOAMI: data=193; break;
        case SCL_ADDR_STATUS: data=summary; break;
        default: break;
    }
    rx[0]=(uint8_t)((previous_address<<2U)|((summary!=0U)?3U:1U));
    rx[1]=(uint8_t)(data>>8U); rx[2]=(uint8_t)data; rx[3]=responseCrc(rx);
    /* ② 再让本帧命令改变器件状态。 */
    if(address==SCL_ADDR_CMD) { summary=(uint16_t)(SCL_STATUS_PWR|SCL_STATUS_MODE_CHANGE); }
    if(address==SCL_ADDR_STATUS) { summary=0U; }   /* 读一次即复位，返回的旧值已在上一步给出 */
    if(arm_on_first_data && !arm_done && (address==SCL_ADDR_ANG_X)) {
        summary=(uint16_t)(SCL_STATUS_PWR|SCL_STATUS_MODE_CHANGE); ++armed_count; arm_done=true;
    }
    previous_address=address; spi_pending=true; return BSP_OK;
}
bool BSP_spiResult(BspSpiDevice device, bsp_status_t *result) {
    CHECK(device==BSP_SPI_SENSOR);
    if(!spi_pending) { return false; }
    spi_pending=false; *result=BSP_OK; return true;
}
void SVC_Log_Printf(svc_log_lvl_t level, char const *fmt, ...) { (void)level; (void)fmt; }
void DRV_Battery_Process(void) { }
bool DRV_Battery_IsDone(void) { return battery_done; }
drv_status_t DRV_Battery_StartSample(void) { battery_done=true; return DRV_OK; }
drv_status_t DRV_Battery_GetSample(DrvBatterySample *out) {
    CHECK(battery_done); battery_done=false; out->voltage_mV=3650; return DRV_OK;
}
void DRV_Battery_Cancel(void) { battery_done=false; }
DrvBatteryDiagnostic DRV_Battery_GetDiagnostic(void) { DrvBatteryDiagnostic d={0}; return d; }
static void testConversion(void) {
    unsigned raw; int32_t previous=-18001;
    CHECK(SCL_RawToAngleCdeg(0)==0);
    CHECK(SCL_RawToAngleCdeg(0xFFFA)==-3); /* 旧代码35996，用户日志359.96°可复现。 */
    CHECK(SCL_RawToAngleCdeg(0x4000)==9000 && SCL_RawToAngleCdeg(0xC000)==-9000);
    CHECK(SCL_RawToAngleCdeg(0x7FFF)==17999 && SCL_RawToAngleCdeg(0x8000)==-18000);
    CHECK(SCL_RawToAngleCdeg(0x0F88)==2184); /* 手册6.1.3固定向量。 */
    for(raw=0;raw<65536U;++raw) {
        int32_t signed_raw=raw<32768U?(int32_t)raw:(int32_t)raw-65536;
        int32_t expected=(int32_t)(((int64_t)signed_raw*9000)/16384);
        int32_t ordered=SCL_RawToAngleCdeg((uint16_t)(raw+32768U));
        CHECK(SCL_RawToAngleCdeg((uint16_t)raw)==expected);
        CHECK(ordered>=previous); previous=ordered;
    }
    puts("PASS all 65536 raw values, signed boundaries, monotonic order and datasheet vector");
}
/** @brief 走真实初始化、14帧采样及四点滤波，不用已经换算好的驱动替身。 */
static SensorCycleResult runBatch(uint16_t const values[4]) {
    SensorCycle cycle; SensorCycleResult out; unsigned i;
    CHECK(!spi_pending); sample_index=UINT32_MAX; memcpy(y_raw,values,sizeof(y_raw));
    App_SensorCycle_Init(&cycle); App_SensorCycle_Request(&cycle,SENSOR_TARGET_CLOUD);
    for(i=0;i<500U;++i) {
        clock_ms+=20; App_SensorCycle_Process(&cycle);
        if(App_SensorCycle_Take(&cycle,&out)) {
            CHECK(out.valid && out.battery_valid && out.sensor.angle_cdeg[0]==2184);
            CHECK(out.sensor.angle_cdeg[2]==9000 && out.sensor.acc_mg[0]==1000 && out.sensor.temp_cC==2509);
            CHECK(!spi_pending); return out;
        }
    }
    CHECK(false); memset(&out,0,sizeof(out)); return out;
}
static void testBatches(void) {
    uint16_t const negative[4]={0xFFFA,0xFFFA,0xFFFA,0xFFFA};
    uint16_t const crossing[4]={0xFFFA,0,0,0};
    uint16_t const alternating[4]={0xFFFA,6,0xFFFA,6};
    uint16_t const positive[4]={6,6,6,6};
    SensorCycleResult result;
    CHECK(DRV_Sensor_Init()==DRV_OK);
    result=runBatch(negative); CHECK(result.sensor.angle_cdeg[1]==-3);
    result=runBatch(crossing); CHECK(result.sensor.angle_cdeg[1]==0);
    result=runBatch(alternating); CHECK(result.sensor.angle_cdeg[1]==0);
    result=runBatch(positive); CHECK(result.sensor.angle_cdeg[1]==3);
    puts("PASS actual SPI pipeline and batches: negative -0.03, crossing 0.00, alternating 0.00, positive 0.03");
}
/** @brief 复现 2026-10-14 现场现象：器件锁存良性位（PWR|MODE_CHANGE = 0x12）后，
 *  手册 5.1.5 规定**每一条**应答都带 RS=11，第一个数据帧就会被判成错误。
 *  旧实现遇到 RS≠01 直接判整批失败、还把这两个良性位当器件故障去重初始化，
 *  于是每个上报周期都白丢一次采样。现在应先读清摘要、重做本点，批次照样成功。 */
static void testBenignStatusRetry(void) {
    uint16_t const positive[4]={6,6,6,6};
    SensorCycleResult result;
    arm_on_first_data=true; arm_done=false; armed_count=0U;
    result=runBatch(positive);
    CHECK(armed_count==1U);                       /* 确认器件真的在作业中途锁存了一次 */
    CHECK(result.valid && result.sensor.angle_cdeg[1]==3);
    arm_on_first_data=false;
    puts("PASS latched benign status (RS=11) is read-cleared and the point retried, batch kept");
}
int main(void) {
    testConversion(); testBatches(); testBenignStatusRetry();
    puts("scl3300 tests: 3 groups passed"); return 0;
}
