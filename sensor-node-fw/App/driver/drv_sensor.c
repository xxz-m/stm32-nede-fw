/*============================================================================
* drv_sensor.c —— 器件驱动层：传感器接口的仿真实现
*
* 层次定位：Driver 层。只依赖 BSP 提供的时间基准（BSP_uptimeMs），不碰 HAL、
* 不包含 qpc.h、不做业务判断。
*
* 为什么是仿真：目标板当前只挂了调试串口（见 docs_sensor/01-项目介绍.md）。
* 真实传感器接入后，用 drv_sht30.c 之类的实现替换本文件（同名接口），
* App / Service 层不需要改动。
*
* 数据特征：随时间缓慢漂移 + 固定周期的确定性“噪声”，便于观察滤波效果，
* 且完全可复现（不依赖随机数）。
*===========================================================================*/
#include "drv_sensor.h"
#include "bsp.h"

static bool s_ready; /* 器件是否已初始化 */

/*--------------------------------------------------------------------------*/
drv_status_t DRV_Sensor_Init(void) {
    s_ready = true;
    return DRV_OK;
}

/*--------------------------------------------------------------------------*/
bool DRV_Sensor_IsReady(void) {
    return s_ready;
}

/*--------------------------------------------------------------------------*/
drv_status_t DRV_Sensor_ReadTemp_cC(int32_t *out) {
    uint32_t ms;
    int32_t drift;
    int32_t noise;

    if (out == (int32_t *)0) {
        return DRV_ERR_PARAM;
    }
    if (!s_ready) {
        return DRV_ERR_NODEV;
    }

    ms = BSP_uptimeMs();

    /* 漂移：每 100 ms 变化 0.01 °C，在 ±1.00 °C 范围内往返 */
    drift = (int32_t)((ms / 100U) % 200U) - 100;
    /* 噪声：±0.05 °C 的确定性抖动，用于验证滤波 */
    noise = (int32_t)((ms / 7U) % 11U) - 5;

    *out = 2350 + drift + noise; /* 基准 23.50 °C */
    return DRV_OK;
}

/*--------------------------------------------------------------------------*/
drv_status_t DRV_Sensor_ReadRh_permil(uint16_t *out) {
    uint32_t ms;
    int32_t value;

    if (out == (uint16_t *)0) {
        return DRV_ERR_PARAM;
    }
    if (!s_ready) {
        return DRV_ERR_NODEV;
    }

    ms = BSP_uptimeMs();

    /* 漂移：每 130 ms 变化 0.1 %RH，在 ±2.0 %RH 内往返 */
    value = 456 + (int32_t)((ms / 130U) % 41U) - 20;

    *out = (uint16_t)value;
    return DRV_OK;
}
