/*============================================================================
* bsp_i2c.h —— 板级支持层：I2C1 主机（非阻塞中断传输）
*
* 层次定位：App → Service → Driver → **BSP** → HAL/CMSIS
*           BSP 是唯一允许接触 HAL 与寄存器的自研层。
*
* 职责：
*   1. 把 HAL 的 I2C 中断传输（Mem_Write_IT / Mem_Read_IT）封装成「启动即返回」
*      的非阻塞接口；调用方靠 BSP_i2cIsBusy() + BSP_i2cLastStatus() 判定结束，
*      或用 BSP_i2cSetSink() 注册的完成回调用事件推进
*   2. 中断使能与 IRQ 入口由本模块负责（见下方 NOTE）
*   3. 总线卡死时的恢复（9 个时钟 + STOP）与诊断计数
*
* NOTE (中断使能为什么放在这里，而不是写进 .ioc)：
*   CubeMX 只有在 .ioc 里勾选了中断时，才会生成 stm32f1xx_it.c 里的
*   I2C1_EV_IRQHandler / I2C1_ER_IRQHandler。本工程不希望为了一个外设再多一次
*   「改 .ioc → GENERATE CODE → 跑 apply_qp_to_keil.ps1」的流程；而启动文件里的
*   中断向量是**弱符号**，在 BSP 里定义同名 handler 即可覆盖它。BSP 恰好也是唯一
*   允许碰寄存器的层，因此 NVIC 使能与优先级放这里既不越界，也不会与生成代码冲突。
*   ⚠ 若将来在 CubeMX 里勾选了 I2C1 的 Event/Error 中断，必须删掉 bsp_i2c.c 里的
*     两个 handler，否则会与 stm32f1xx_it.c 的定义重复（链接期 multiply defined）。
*   ⚠ 本模块必须在 QF_init() 之后调用：QV_init() 会把所有中断优先级统一刷成安全值 3，
*     本模块随后把 I2C1 设成 BSP_I2C_IRQ_PRIO(5)，两者都满足「>= 3」。当前调用链是
*     main() → QF_init() → BSP_init()（内部调用 BSP_i2cInit()），顺序是对的。
*
* 约束（见 docs_sensor/07-接口约定.md#BSP 层）：
*   - 公开头文件里不出现 HAL 类型（I2C_HandleTypeDef 不外露）
*   - 从机地址一律用「8 位地址字节」表示（已经左移，最低位是 R/W），与 HAL 一致；
*     例如 AT24C04 的 A1/A2 接地时是 0xA0（写）/ 0xA1（读）
*   - 每个 ISR 安全的函数都在注释里写明由谁调用
*
* 目标硬件：STM32F103RCT6，I2C1 = PB6(SCL) / PB7(SDA)，100 kHz
*===========================================================================*/
#ifndef BSP_I2C_H
#define BSP_I2C_H

#include <stdint.h>
#include <stdbool.h>

/* 全局配置（所有层都可依赖，见 AGENTS.md 第 5.1 节例外条款）*/
#include "app_config.h"
/* bsp_status_t 定义在 bsp.h；本头文件被 bsp.h 反向包含时靠包含卫士避免递归。
* 因此单独包含本文件（不先包含 bsp.h）也能编译。 */
#include "bsp.h"

/*! I2C1 中断抢占优先级（CMSIS 记法，数值越大优先级越低）。
*  必须 >= QF_AWARE_ISR_CMSIS_PRI(3)，否则中断里不允许投递事件。 */
#define BSP_I2C_IRQ_PRIO    (5U)

/** @brief 一次中断传输结束（成功或失败）的通知回调。
* @note 运行在 I2C1 中断上下文：只允许存数据、投递事件与重装外设；
*       禁止打印、禁止解析、禁止再发起一次传输。
*/
typedef void (*BspI2cSink)(void);

/* 初始化 -------------------------------------------------------------------*/

/*! 初始化 I2C1 主机：使能事件/错误中断、清理卡死的总线。
* @note 必须在 MX_I2C1_Init()（main.c 已调用）与 QF_init() 之后调用；
*       当前由 BSP_init() 负责调用。
* @return BSP_OK / BSP_ERR_IO（外设未初始化）
*/
bsp_status_t BSP_i2cInit(void);

/*! 注册「传输结束」回调；传 NULL 表示注销。
* @note 由 App 层装配点调用，BSP 自身不含应用逻辑（与 BSP_uartRxSetSink 同形）。
*/
void BSP_i2cSetSink(BspI2cSink sink);

/* 中断传输（非阻塞） -------------------------------------------------------*/

/*! 启动一次「写器件内部地址 + 连续写数据」的中断传输，立即返回。
* @param devAddr8 从机地址字节（已左移；AT24C04 为 0xA0 / 0xA2）
* @param memAddr  器件内部地址（I2C1 用的是 8 位内存地址模式）
* @param buf      数据源；**传输期间必须保持有效**（中断里才会读完）
* @param len      字节数，取值 1..255
* @return BSP_OK 已启动 / BSP_ERR_BUSY 上一次还没结束 / BSP_ERR_PARAM / BSP_ERR_IO
*/
bsp_status_t BSP_i2cMemWriteIt(uint8_t devAddr8, uint8_t memAddr,
                               uint8_t const *buf, uint16_t len);

/*! 启动一次「写器件内部地址 + 连续读数据」的中断传输，立即返回。
* @param buf 目标缓冲；传输期间必须保持有效
* @return 同 BSP_i2cMemWriteIt()
*/
bsp_status_t BSP_i2cMemReadIt(uint8_t devAddr8, uint8_t memAddr,
                              uint8_t *buf, uint16_t len);

/*! 是否有中断传输正在进行。
* @note 这是调用方判断「上一拍的动作完成没有」的唯一依据；返回 false 之后
*       还要看 BSP_i2cLastStatus() 才知道是成功还是失败。
*/
bool BSP_i2cIsBusy(void);

/*! 最近一次中断传输的结束状态：BSP_OK 或 BSP_ERR_IO。 */
bsp_status_t BSP_i2cLastStatus(void);

/* 诊断用 -------------------------------------------------------------------*/

/*! 有界阻塞地探测某个地址上是否有器件应答（ACK）。
* @param devAddr8  从机地址字节（已左移）
* @param timeoutMs 单次探测的超时上限（毫秒）
* @return BSP_OK 有应答 / BSP_ERR_TIMEOUT 无应答 / BSP_ERR_BUSY 总线忙 / BSP_ERR_PARAM
* @warning 这是本模块唯一的阻塞接口，只给「I2C 扫描」这类诊断用。单次预算由
*          timeoutMs 决定（调用点传 1~2 ms），**禁止在同一个 AO 处理函数里连续
*          多次调用**（阻塞预算见 07-接口约定#时序与阻塞预算）。
*/
bsp_status_t BSP_i2cProbe(uint8_t devAddr8, uint32_t timeoutMs);

/*! 总线恢复：把 PB6/PB7 当普通开漏 GPIO 打 9 个时钟 + 一个 STOP，再重新初始化外设。
* @return BSP_OK / BSP_ERR_IO
* @note 只在总线卡死（从机把 SCL/SDA 一直拉低、BUSY 标志不落）时调用，属于最后手段。
*       执行期间会短暂占用约 0.3 ms，且会重置 I2C 外设状态。
*/
bsp_status_t BSP_i2cBusRecover(void);

/*! 中断传输出错累计次数（诊断用）。 */
uint32_t BSP_i2cErrorCount(void);

/*! 总线恢复执行次数（诊断用）。 */
uint32_t BSP_i2cRecoverCount(void);

/*! 中断传输完成次数（诊断用）。 */
uint32_t BSP_i2cXferCount(void);

#endif /* BSP_I2C_H */
