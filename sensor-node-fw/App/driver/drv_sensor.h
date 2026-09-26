/*============================================================================
* drv_sensor.h —— 器件驱动层：传感器统一语义接口
*
* 层次定位：App → Service → **Driver(器件驱动层)** → BSP → HAL/CMSIS
*
* 约定（见 docs_sensor/07-接口约定.md#Driver 层）：
*   - 量纲写进函数名，驱动内部不做物理量换算（换算曲线属于 Service 层）
*   - 输出参数放最后；只读入参用 const；返回值只表达成功 / 失败
*   - 只允许调用 BSP 提供的接口，不得 #include "qpc.h"，不得直接碰寄存器或 HAL
*   - 必须能在 ARM Compiler 6 下编译
*
* 当前实现为「仿真数据源」（板上目前只挂了调试串口，未接真实传感器）。
* 接入真实器件时新增 drv_sht30.c 等实现同名接口，App/Service 层调用点不变。
*===========================================================================*/
#ifndef DRV_SENSOR_H
#define DRV_SENSOR_H

#include <stdint.h>
#include <stdbool.h>

/* Driver 层公共类型（drv_status_t）。
* NOTE (2026-09-27 提取)：原先本文件自己定义 drv_status_t，第二个驱动
* （drv_at24c04）再引用它就变成「存储驱动包含传感器驱动头文件」的假依赖，
* 故提到 drv_types.h，本文件改为包含它；对外类型与取值不变。 */
#include "drv_types.h"

/*! 初始化器件（仿真实现无总线操作；真实器件在此做上电时序与自检）。
* @return DRV_OK / DRV_ERR_NODEV / DRV_ERR_TIMEOUT
*/
drv_status_t DRV_Sensor_Init(void);

/*! 读取温度。
* @param out 输出：0.01 °C 标度（例如 2345 表示 23.45 °C）
* @return DRV_OK / DRV_ERR_PARAM / DRV_ERR_BUS / DRV_ERR_TIMEOUT / DRV_ERR_NODEV
*/
drv_status_t DRV_Sensor_ReadTemp_cC(int32_t *out);

/*! 读取相对湿度。
* @param out 输出：0.1 %RH 标度（例如 456 表示 45.6 %RH）
* @return 同 DRV_Sensor_ReadTemp_cC()
*/
drv_status_t DRV_Sensor_ReadRh_permil(uint16_t *out);

/*! 器件是否已初始化且可用。 */
bool DRV_Sensor_IsReady(void);

#endif /* DRV_SENSOR_H */
