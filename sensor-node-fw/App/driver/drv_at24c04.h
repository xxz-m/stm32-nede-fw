/*============================================================================
* drv_at24c04.h —— 器件驱动层：AT24C04 I2C EEPROM（非阻塞）
*
* 层次定位：App → Service → **Driver(器件驱动层)** → BSP → HAL/CMSIS
*
* 目标器件：AT24C04（4 Kbit = 512 B，页写 16 B，内部写周期 tWR 约 5 ms）
* 总线    ：I2C1（PB6/PB7，100 kHz），经 BSP_i2c_* 访问
* 从机地址：A1/A2 接地 → 7 位 0x50（由 APP_CFG_STORE_I2C_ADDR 配置）
*
* ⚠ 本驱动是**非阻塞**的，接口形态与常见的阻塞式 EEPROM 驱动不同：
*     发起：DRV_AT24C04_ReadStart() / WriteStart() / ProbeStart()
*     推进：调用方在自己的周期事件里反复调用 DRV_AT24C04_Process()
*     查询：DRV_AT24C04_State() + DRV_AT24C04_Result()
*   任何一步都不会原地等待总线，也不会 HAL_Delay()；页拆分、写周期等待、
*   重试与总线恢复都在 Process() 里按毫秒时基推进。原因见 AGENTS.md 5.2：
*   QV 是协作式内核，驱动里一旦忙等，整个系统停摆。
*
* 工作流程（以「写 100 字节」为例，全部由 Process() 分拍推进）：
*   ISSUE：按「不跨 16 B 页、不跨 256 B 块」切成一段，启动一次中断写 → WAIT
*   WAIT ：等 BSP_i2cIsBusy() 落下，成功后 pos += chunk
*   HOLD ：等待 EEPROM 内部写周期（APP_CFG_STORE_WRITE_CYCLE_MS）→ 下一段 ISSUE
*   最后一段写完后 → DONE；任何一段重试用尽 → 补时钟恢复总线 → 再失败才 FAILED
*
* AT24C04 的两处器件特性（都在本文件内部消化，上层看不到）：
*   1. 第 9 位地址：512 B 需要 9 位地址，超过 255 的那一位由**器件地址字节的 P0 位**
*      承载，因此 0x00..0xFF 用 0xA0、0x100..0x1FF 用 0xA2。连续读也必须在 256 B
*      块边界断开重发地址，否则会回卷到块首。
*   2. 写不能跨页：一页 16 B，跨页写会「回卷」覆盖本页开头。因此写必须按页拆。
*
* 本层不得 #include "qpc.h"，也不做 I/O 重传等待之外的任何逻辑。
*===========================================================================*/
#ifndef DRV_AT24C04_H
#define DRV_AT24C04_H

#include <stdint.h>
#include <stdbool.h>
#include "drv_types.h"

/*! 作业状态（由调用方轮询）。 */
typedef enum {
    DRV_AT24C04_STATE_IDLE = 0, /*!< 空闲：没有进行中的作业，也没有未取走的结果 */
    DRV_AT24C04_STATE_BUSY,     /*!< 进行中：继续调 Process()                     */
    DRV_AT24C04_STATE_DONE,     /*!< 成功结束：结果已可取                         */
    DRV_AT24C04_STATE_FAILED    /*!< 失败结束：原因见 DRV_AT24C04_Result()        */
} drv_at24c04_state_t;

/*! 器件容量（字节），供上层计算槽位。 */
uint16_t DRV_AT24C04_Capacity(void);

/*! 页写大小（字节），供上层理解写粒度。 */
uint16_t DRV_AT24C04_PageSize(void);

/*! 模块初始化（清零内部状态；不访问总线，因此不会阻塞）。
* @note 上电调用一次即可；开始每个新作业前不需要重复调用。
*/
void DRV_AT24C04_Init(void);

/*! 推进当前作业。必须被周期性调用（每拍一次），直到状态离开 BUSY。
* @note 这是本驱动的「心跳」：页拆分、写周期等待、重试、总线恢复都在里面推进。
*       可在 AO 的周期事件、I2C 完成事件里调用，代价是微秒级。
*/
void DRV_AT24C04_Process(void);

/*! 发起一次「器件是否存在」的探测（读 1 字节，看有没有 ACK）。
* @return DRV_OK 已开始 / DRV_ERR_BUSY 上一个作业未取走结果 / 其它 = 入参问题
* @note 失败时会被判成 DRV_ERR_NODEV（不是总线错误）。结果用 State()/Result() 取。
*/
drv_status_t DRV_AT24C04_ProbeStart(void);

/*! 发起一次连续读。
* @param addr 器件内起始地址（0..511）
* @param buf  目标缓冲；**作业结束前必须保持有效**
* @param len  字节数（1..511）
* @return DRV_OK 已开始 / DRV_ERR_BUSY / DRV_ERR_PARAM（越界或空指针）
*/
drv_status_t DRV_AT24C04_ReadStart(uint16_t addr, uint8_t *buf, uint16_t len);

/*! 发起一次连续写（内部按页拆分，不需要调用方关心页边界）。
* @param addr 器件内起始地址（0..511）
* @param buf  数据源；**作业结束前必须保持有效**
* @param len  字节数（1..511）
* @return 同 DRV_AT24C04_ReadStart()
* @note 每页之间自动插入内部写周期等待；调用方只需要不停调 Process()。
*/
drv_status_t DRV_AT24C04_WriteStart(uint16_t addr, uint8_t const *buf, uint16_t len);

/*! 当前作业状态。 */
drv_at24c04_state_t DRV_AT24C04_State(void);

/*! 最近一次作业的结果（DONE 时为 DRV_OK；FAILED 时为具体原因）。 */
drv_status_t DRV_AT24C04_Result(void);

/*! 结果码转 ASCII 短名，便于日志（不打印中文，见 AGENTS.md 5.2）。 */
char const *DRV_AT24C04_ErrStr(drv_status_t st);

#endif /* DRV_AT24C04_H */
