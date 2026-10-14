/* App 层：SCL3300 与电池同批采集，STM32F103，QV 非阻塞推进。
 * 每批重建滤波窗口，避免分钟级采样混入上一小时的历史角度。 */
#include "sensor_cycle.h"
#include "app_config.h"
#include "bsp.h"
#include "drv_battery.h"
#include "svc_log.h"
#include <string.h>

void App_SensorCycle_Init(SensorCycle *c) { memset(c, 0, sizeof(*c)); }
void App_SensorCycle_Request(SensorCycle *c, uint8_t targets) {
    if (c->active) { c->result.targets |= targets; }
    else { c->pending |= targets; }
}
void App_SensorCycle_Cancel(SensorCycle *c) {
#if APP_CFG_BATTERY_ENABLE != 0U
    DRV_Battery_Cancel();
#endif
    c->active = false; c->done = false; c->pending = 0U;
}
/** @brief 单次失败只结束本批，不把上一次的测量伪装成新数据。
 *
 * NOTE (诊断字段，2026-10-14 现场加)：失败日志必须带驱动错误码与批次进度。
 *   原因：驱动把三条完全不同的路径——数据帧 CRC 不符、器件回 RS≠01、BSP 事务错
 *   （DMA TEIF/OVR/MODF 或单帧 100 ms 超时）——**都收敛成 DRV_ERR_BUS**，而
 *   DRV_ERR_TIMEOUT 是 1500 ms 作业超时，排查方向与前三者完全不同。
 *   只打 `reason=scl3300` 时这四类在日志上完全同形，用户实测就是被这一条挡住。
 *   `ready` 是失败瞬间器件是否仍处于 SCL_READY（0 = 已进恢复/重初始化流程），
 *   `count` 是本批已完成的有效采样点数（0 = 第一个角度帧就失败），
 *   `elapsed` 是距本批启动的毫秒数（对 1500 ms/8000 ms 两级期限做参照）。 */
static void failCycle(SensorCycle *c, char const *reason, drv_status_t drv) {
#if APP_CFG_BATTERY_ENABLE != 0U
    DRV_Battery_Cancel();
#endif
    c->result.valid = false; c->active = false; c->done = true;
    LOG_W("sensor: sample failed reason=%s drv=%d ready=%u count=%u elapsed=%lu ms",
          reason, (int)drv, (unsigned)DRV_Sensor_IsReady(), (unsigned)c->count,
          (unsigned long)(BSP_uptimeMs() - c->started_ms));
}
/** @brief 电池故障仅省略电压，倾角仍可正常上报。 */
static void collectBattery(SensorCycle *c) {
#if APP_CFG_BATTERY_ENABLE != 0U
    if (!c->battery_done && DRV_Battery_IsDone()) {
        DrvBatterySample sample;
        drv_status_t st = DRV_Battery_GetSample(&sample);
        c->battery_done = true;
        c->result.battery_valid = st == DRV_OK;
        if (st == DRV_OK) { c->result.battery_mV = sample.voltage_mV; }
        else {
            DrvBatteryDiagnostic d = DRV_Battery_GetDiagnostic();
            LOG_W("battery: sample failed reason=%s adc=%d", d.reason, (int)d.adc_status);
        }
    }
#else
    c->battery_done = true;
#endif
}
void App_SensorCycle_Process(SensorCycle *c) {
    unsigned axis;
    BSP_spiProcess();
    DRV_Sensor_Process();
#if APP_CFG_BATTERY_ENABLE != 0U
    DRV_Battery_Process();
#endif
    if (c->done) { return; }
    if (!c->active) {
        /* 超时/取消可能留下已完成的 SPI 作业，必须消费后才可再次发起。 */
        if (DRV_Sensor_IsDone()) { DrvSensorSample unused; (void)DRV_Sensor_GetSample(&unused); }
        if (c->pending == 0U) { return; }
        memset(&c->result, 0, sizeof(c->result));
        c->result.targets = c->pending; c->pending = 0U;
        c->active = true; c->started_ms = BSP_uptimeMs(); c->count = 0U;
        c->sensor_pending = false; c->battery_started = false; c->battery_done = false;
        for (axis = 0U; axis < DRV_SENSOR_AXIS_COUNT; ++axis) {
            (void)SVC_Filter_MavgInit(&c->filter[axis], APP_CFG_SENSOR_MAVG_LEN);
        }
    }
    if ((uint32_t)(BSP_uptimeMs() - c->started_ms) >= APP_CFG_SENSOR_CYCLE_TIMEOUT_MS) {
        failCycle(c, "cycle_timeout", DRV_OK); return;
    }
    if (!c->battery_started && DRV_Sensor_IsReady()) {
        c->battery_started = true;
#if APP_CFG_BATTERY_ENABLE != 0U
        if (DRV_Battery_StartSample() != DRV_OK) {
            c->battery_done = true;
            LOG_W("battery: start failed");
        }
#endif
    }
    collectBattery(c);
    if (c->sensor_pending && DRV_Sensor_IsDone()) {
        DrvSensorSample sample;
        drv_status_t st = DRV_Sensor_GetSample(&sample);
        c->sensor_pending = false;
        if (st != DRV_OK) { failCycle(c, "scl3300", st); return; }
        c->result.sensor = sample;
        for (axis = 0U; axis < DRV_SENSOR_AXIS_COUNT; ++axis) {
            c->result.sensor.angle_cdeg[axis] = SVC_Filter_MavgPush(&c->filter[axis], sample.angle_cdeg[axis]);
        }
        ++c->count;
    }
    /* 超时后上一笔 SPI 可能迟到；只清理不属于本批的终态，不能把它当新样本。 */
    if (!c->sensor_pending && DRV_Sensor_IsDone()) {
        DrvSensorSample unused;
        (void)DRV_Sensor_GetSample(&unused);
    }
    if ((c->count < APP_CFG_SENSOR_MAVG_LEN) && !c->sensor_pending && DRV_Sensor_IsReady()) {
        drv_status_t st = DRV_Sensor_StartSample();
        if (st == DRV_OK) { c->sensor_pending = true; }
        else if (st != DRV_ERR_BUSY) { failCycle(c, "scl_start", st); return; }
    }
    if ((c->count == APP_CFG_SENSOR_MAVG_LEN) && c->battery_done) {
        c->result.valid = true; c->active = false; c->done = true;
    }
}
bool App_SensorCycle_Take(SensorCycle *c, SensorCycleResult *result) {
    if (!c->done || result == (SensorCycleResult *)0) { return false; }
    *result = c->result; c->done = false; return true;
}
