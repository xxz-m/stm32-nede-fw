/*============================================================================
* drv_scl3300.h —— 器件驱动层：SCL3300-D01 三轴倾角仪接口
*
* 层次定位：App → Service → **Driver(器件驱动层)** → BSP → HAL/CMSIS
*
* 当前实现：**SCL3300-D01**（Murata 三轴数字倾角仪，SPI 接口）。
*   手册：SCL3300-D01 Doc.No. 4921 Rev. 4。
*   片选 PA4，挂在共享 SPI1 上（与 W25Q32 共用 PA5/PA6/PA7）。
* 目标硬件：STM32F103RCT6；所属内核：QP/C 6.9.1 QV，接口只在 AO 上下文调用。
*
* NOTE (2026-10-10 换掉仿真源)：本文件原先声明的是「温度 + 湿度」接口，对应一个
*   仿真数据源。按用户要求接入真实 SCL3300 后：
*     ② **湿度接口被移除**——SCL3300 没有湿度通道，保留它只会让上层以为有数据；
*     ③ 改为三轴倾角 + 加速度 + 片内温度。
*
* 约定（见 docs_sensor/07-接口约定.md#Driver 层）：
*   - 量纲写进函数名，驱动内部不做物理量换算之外的业务判断
*   - 输出参数放最后；只读入参用 const；返回值只表达成功 / 失败
*   - 只允许调用 BSP 提供的接口，不得 #include "qpc.h"，不得直接碰寄存器或 HAL
*   - 非阻塞：超过毫秒级的等待一律拆成「发起 → 调用方反复 Process → 取结果」
*   - 必须能在 ARM Compiler 6 下编译
*===========================================================================*/
#ifndef DRV_SCL3300_H
#define DRV_SCL3300_H

#include <stdint.h>
#include <stdbool.h>

/* Driver 层公共类型（drv_status_t）。 */
#include "drv_types.h"

/*--------------------------------------------------------------- 物理量标度 */
/*! 倾角标度：0.01°（例如 1234 表示 12.34°）。
*  NOTE (类型与实现)：输出类型为 int32_t，但当前原始值按 uint16_t 换算；
*  下列范围宏不参与当前驱动裁剪，也不代表已验证负角度转换。 */
#define DRV_SENSOR_ANGLE_CDEG_MIN   (-1000)   /* Mode 3/4 量程 ±10.00° */
#define DRV_SENSOR_ANGLE_CDEG_MAX   ( 1000)

/*! 温度标度：0.01 °C（例如 2350 表示 23.50 °C）。
*  NOTE：这是**芯片自身温度**（SCL3300 内部温度传感器），用作温度补偿与诊断，
*  不是环境气温——芯片自热会让它略高于环境。 */
#define DRV_SENSOR_TEMP_CC_SCALE    (100)

/*! 加速度标度：毫 g（mg，1 g = 1000 mg，表示加速度而非质量）。有符号。 */
#define DRV_SENSOR_ACC_MG_SCALE     (1000)

/*! 三轴编号：与器件 X/Y/Z 对应。 */
typedef enum {
    DRV_SENSOR_AXIS_X = 0,
    DRV_SENSOR_AXIS_Y,
    DRV_SENSOR_AXIS_Z,
    DRV_SENSOR_AXIS_COUNT
} drv_scl3300_axis_t;

/*! 一次采样的结果快照（全部为已换算的工程量，可直接上报）。 */
typedef struct {
    int32_t angle_cdeg[DRV_SENSOR_AXIS_COUNT]; /*!< 三轴倾角，0.01°        */
    int32_t acc_mg[DRV_SENSOR_AXIS_COUNT];     /*!< 三轴加速度，mg         */
    int32_t temp_cC;                           /*!< 片内温度，0.01 °C      */
    uint16_t status;                           /*!< 最近一次读到的 STATUS  */
    bool    status_valid;                      /*!< 是否已取得 STATUS；有效不等于无故障 */
} DrvSensorSample;

/*--------------------------------------------------------------- 生命周期 */
/** @brief 初始化软件状态，不发起任何总线事务；片选登记由 BSP_spiInit() 完成。
*  真正的器件初始化由 DRV_Sensor_Process() 推进；不要在事务未收回时调用本函数。
* @return 当前实现固定返回 DRV_OK；不代表已识别器件
*/
drv_status_t DRV_Sensor_Init(void);

/*! 器件是否已完成上电时序（SW Reset → 3 ms → 设模式 → 使能角度 → 自检）。
*  @return true = 自检通过、可以取样。 */
bool DRV_Sensor_IsReady(void);

/** @brief 推进驱动状态机，必须在 AO 中周期调用；当前推进周期为 20 ms。
*
*  调用顺序：先 BSP_spiProcess() 更新 DMA 完成状态，再调用本函数收帧和推进。
*  本函数不等待、不自行创建定时器，也不会仅因调用 StartSample 就自动运行。
*  NOTE (为什么必须反复调用)：一次采样需要读 7 个寄存器，每个寄存器按手册
*  5.1.2 的错帧协议在本实现中采用两帧一组，共 14 帧；帧间 CSB 为高 ≥10 µs。
*  这些等待全部用「时间戳 + 下一次调用」实现，绝不在函数里忙等——QV 是协作式
*  单栈内核，任何一次不返回都会停掉整个系统。
*/
void DRV_Sensor_Process(void);

/*--------------------------------------------------------------- 采样作业 */
/** @brief 发起一次采样（非阻塞）；调用方应先取走上一份成功或失败结果。
* @return DRV_OK = 已受理；DRV_ERR_BUSY = 上一次作业还没结束；
*         DRV_ERR_NODEV = 器件未就绪
*/
drv_status_t DRV_Sensor_StartSample(void);

/** @brief 作业是否进入成功或失败终态；true 不表示采样一定成功，需调用 GetSample 判定。 */
bool DRV_Sensor_IsDone(void);

/** @brief 取走成功快照或失败错误码；两种终态消费后 IsDone 都返回 false。
* @param out 输出：仅返回 DRV_OK 时写入完整快照，其他返回值保持原缓冲不变
* @return DRV_OK = 成功；DRV_ERR_PARAM = 空指针；DRV_ERR_BUSY = 无成功/失败结果可取；
*         DRV_ERR_BUS / DRV_ERR_TIMEOUT = 当前作业失败，错误已消费
*/
drv_status_t DRV_Sensor_GetSample(DrvSensorSample *out);

/** @brief 查询最近保存的错误码，不消费结果；新采样或重新初始化会重置，不是历史错误计数。 */
drv_status_t DRV_Sensor_LastError(void);

#endif /* DRV_SCL3300_H */
