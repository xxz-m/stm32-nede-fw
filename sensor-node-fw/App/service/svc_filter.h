/*============================================================================
* svc_filter.h —— 服务层：数字滤波
*
* 层次定位：App(应用层) → **Service(服务层)** → Driver → BSP → HAL/CMSIS
*
* 约定（见 docs_sensor/07-接口约定.md）：
*   - 无状态函数优先：SVC_Filter_Median3() 是纯函数
*   - 需要状态时，状态由调用方（App 层的 AO）持有并作为第一个参数传入，
*     Service 内部不定义全局变量，便于测试与重入
*   - 本层不得 #include "qpc.h"，也不得调用总线或做 I/O 等待
*   - 物理量用整数标度，标度写在调用点（例如 0.01 °C 用 int32_t 表示）
*===========================================================================*/
#ifndef SVC_FILTER_H
#define SVC_FILTER_H

#include <stdint.h>
#include <stdbool.h>

/* 滑动平均窗口的最大长度（缓冲区由此决定，静态分配） */
#define SVC_FILTER_MAVG_MAX (8U)

/*! 滑动平均滤波器状态：由调用方持有并传入。
* @note 用 int32_t 累加和，窗口与样本量级需保证不溢出（本工程为温度 0.01 °C，
*       量级 ~10^4，窗口 8，远小于 int32_t 上限）。
*/
typedef struct {
    int32_t buf[SVC_FILTER_MAVG_MAX]; /* 样本环形缓冲           */
    int32_t sum;                     /* 运行和，避免每次遍历   */
    uint8_t len;                     /* 窗口长度 1..MAVG_MAX   */
    uint8_t idx;                     /* 下一个写入位置         */
    uint8_t filled;                  /* 已填入的样本数         */
} SvcMavg;

/*! 中值滤波（三点）：返回三个数的中位数。无状态纯函数。 */
int32_t SVC_Filter_Median3(int32_t a, int32_t b, int32_t c);

/*! 初始化滑动平均滤波器。
* @param me  调用方持有的状态
* @param len 窗口长度，取值 1..SVC_FILTER_MAVG_MAX
* @return true 参数合法；false 参数非法（状态保持清零）
*/
bool SVC_Filter_MavgInit(SvcMavg *me, uint8_t len);

/*! 送入一个样本并返回当前平均值（未填满时按已填样本数求平均）。
* @note me 为 NULL 或未初始化时直接返回 sample，不做除法。
*/
int32_t SVC_Filter_MavgPush(SvcMavg *me, int32_t sample);

#endif /* SVC_FILTER_H */
