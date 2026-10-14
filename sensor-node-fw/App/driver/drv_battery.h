/*============================================================================
 * drv_battery.h —— Driver 层：单节锂电池开关分压采样
 * 目标硬件：PC1 控制 PMOS，PC0 接 10k/10k 分压；所属内核：QV，非阻塞。
 * 仅依赖 BSP，原始值与电压快照归调用者；不估算剩余电量百分比。
 *===========================================================================*/
#ifndef DRV_BATTERY_H
#define DRV_BATTERY_H
#include <stdint.h>
#include <stdbool.h>
#include "drv_types.h"

typedef struct {
    uint16_t raw;       /* 分压节点的 ADC 平均原始值，0..4095。 */
    uint16_t vref_raw;  /* VREFINT 平均原始值，用来估算 VDDA。 */
    uint16_t vdda_mV;   /* 基于内部参考典型值估算，尚未逐板校准。 */
    uint16_t voltage_mV;/* 电池电压；不是 PC0 节点电压。 */
} DrvBatterySample;

/** @brief 最近一次作业的诊断；部分采样也保留，不能当作有效电池电压。 */
typedef struct {
    char const *reason; /* 静态 ASCII 原因标签，初始/无错误为 none。 */
    int adc_status;     /* 最近一次 BSP ADC 状态码的数值，不暴露 BSP 类型。 */
    bool reference;    /* 最近一次发起或收取的是内部参考通道。 */
    uint8_t bat_count;
    uint8_t ref_count;
    uint16_t raw;       /* 已采样值的平均；raw_range 时为非法读数。 */
    uint16_t vref_raw;
    uint32_t vdda_mV;   /* 保留完整换算值，避免越界后被 uint16_t 截断。 */
    uint32_t voltage_mV;
} DrvBatteryDiagnostic;

/** @brief 初始化软件与 ADC，后续必须周期调用 Process 推进校准。 */
drv_status_t DRV_Battery_Init(void);
/** @brief 开启一次作业；忙或终态未取时不覆盖原作业。 */
drv_status_t DRV_Battery_StartSample(void);
/** @brief 推进稳定等待、ADC 采样、平均及换算，任何路径都不阻塞。 */
void DRV_Battery_Process(void);
/** @brief 成功或失败的终态是否待取。 */
bool DRV_Battery_IsDone(void);
/** @brief 消费成功或失败终态；失败不改输出，空指针不消费结果。 */
drv_status_t DRV_Battery_GetSample(DrvBatterySample *sample);
/** @brief 返回诊断快照，不消费终态；保留至下次成功发起作业或初始化。 */
DrvBatteryDiagnostic DRV_Battery_GetDiagnostic(void);
/** @brief 停止作业并释放 PC1 为高阻；用于退出采样状态。 */
void DRV_Battery_Cancel(void);
#endif
