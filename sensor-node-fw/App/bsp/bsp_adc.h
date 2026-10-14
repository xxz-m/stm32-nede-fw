/*============================================================================
 * bsp_adc.h —— BSP 层：ADC1 非阻塞转换与电池采样开关
 * 目标硬件：STM32F103RCT6，PC0=ADC1_IN10，PC1=低有效电池采样控制。
 * 所属内核：QV；仅由 AO_Sensor 调用，不在中断中调用，不暴露 HAL 类型。
 *===========================================================================*/
#ifndef BSP_ADC_H
#define BSP_ADC_H
#include <stdint.h>
#include <stdbool.h>
/* 由 bsp.h 在 bsp_status_t 定义之后聚合；与现有 SPI 子模块约定一致。 */

typedef enum { BSP_ADC_BATTERY = 10, BSP_ADC_VREFINT = 17 } BspAdcChannel;

/** @brief 初始化 ADC1，校准由 Process 推进；不得重复初始化进行中的事务。 */
bsp_status_t BSP_adcInit(void);
/** @brief 推进上电、校准、转换与超时，每次立即返回。 */
void BSP_adcProcess(void);
/** @brief 受理一笔单通道转换；成功仅表示已受理，结果需由 Result 收取。 */
bsp_status_t BSP_adcStart(BspAdcChannel channel);
/** @brief 收取并消费一次终态；成功时填写 12 位原始值，失败不改 raw。 */
bool BSP_adcResult(bsp_status_t *result, uint16_t *raw);
/** @brief 取消本模块事务并关闭 ADC/VREFINT；初始化校准进行中时不打断。 */
void BSP_adcCancel(void);
/** @brief true 将 PC1 拉低，false 高阻；HOLD_LOW 测试宏开启时一律保持低。 */
void BSP_batteryGateSet(bool enabled);
#endif
