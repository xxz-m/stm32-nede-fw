/*============================================================================
 * drv_storage.h —— Driver 层：非阻塞 NOR 存储接口，与具体芯片解耦
 * 目标硬件：由后端选择，当前 W25Q32JV；内核无关，不依赖 QP/HAL。
 * NOTE (语义)：Program 只编程已擦除区域，不模拟 EEPROM 覆盖写；Erase 独立显式发起。
 * 同时只允许一个作业。State 离开 BUSY 后才能复用缓冲，后续 Start 覆盖旧结果。
 *===========================================================================*/
#ifndef DRV_STORAGE_H
#define DRV_STORAGE_H
#include <stdint.h>
#include <stdbool.h>
#include "drv_types.h"
typedef enum { DRV_STORAGE_IDLE = 0, DRV_STORAGE_BUSY, DRV_STORAGE_DONE, DRV_STORAGE_FAILED } DrvStorageState;
typedef struct { uint32_t capacity; uint32_t erase_size; uint16_t page_size; } DrvStorageGeometry;
/** @brief 初始化后端状态，不访问总线。 */
void DRV_Storage_Init(void);
/** @brief 取后端容量和擦除/编程粒度。 */
DrvStorageGeometry DRV_Storage_Geometry(void);
/** @brief 推进一次作业；由持有作业的 AO 周期调用，不等待器件 BUSY。 */
void DRV_Storage_Process(void);
/** @brief 先等忙状态结束，再读 JEDEC ID；不改保护位、不全片擦除。 */
drv_status_t DRV_Storage_ProbeStart(void);
/** @brief 发起读；32 位地址，缓冲必须保持有效到完成，拒绝越界。 */
drv_status_t DRV_Storage_ReadStart(uint32_t address, uint8_t *data, uint32_t length);
/** @brief 发起编程，自动按页拆分；调用者负责确保目标已擦除。 */
drv_status_t DRV_Storage_ProgramStart(uint32_t address, uint8_t const *data, uint32_t length);
/** @brief 擦除整扇区；地址和长度均必须按 erase_size 对齐。 */
drv_status_t DRV_Storage_EraseStart(uint32_t address, uint32_t length);
/** @brief 查询当前作业状态与结果；失败后若芯片仍 BUSY，新作业也先等待它完成。 */
DrvStorageState DRV_Storage_State(void);
drv_status_t DRV_Storage_Result(void);
#endif
