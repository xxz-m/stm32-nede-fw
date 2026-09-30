/* Driver 层：选择存储后端，当前 W25Q32JV；内核无关，禁止依赖 QP/HAL。 */
#include "drv_storage.h"
#include "drv_w25q32.h"
/** @brief 初始化后端，不访问总线。 */
void DRV_Storage_Init(void) { DRV_W25Q32_Init(); }
/** @brief 返回当前后端几何参数。 */
DrvStorageGeometry DRV_Storage_Geometry(void) {
    DrvStorageGeometry const g = { APP_CFG_FLASH_CAPACITY, APP_CFG_FLASH_SECTOR_SIZE, APP_CFG_FLASH_PAGE_SIZE };
    return g;
}
/** @brief 推进后端状态机。 */
void DRV_Storage_Process(void) { DRV_W25Q32_Process(); }
/** @brief 异步探测器件。 */
drv_status_t DRV_Storage_ProbeStart(void) { return DRV_W25Q32_Start(W25_PROBE, 0U, 0, 0, 0U); }
/** @brief 异步连续读。 */
drv_status_t DRV_Storage_ReadStart(uint32_t a, uint8_t *p, uint32_t n) { return DRV_W25Q32_Start(W25_READ, a, p, 0, n); }
/** @brief 异步按页编程。 */
drv_status_t DRV_Storage_ProgramStart(uint32_t a, uint8_t const *p, uint32_t n) { return DRV_W25Q32_Start(W25_PROGRAM, a, 0, p, n); }
/** @brief 异步扇区擦除。 */
drv_status_t DRV_Storage_EraseStart(uint32_t a, uint32_t n) { return DRV_W25Q32_Start(W25_ERASE, a, 0, 0, n); }
/** @brief 查询作业状态。 */
DrvStorageState DRV_Storage_State(void) { return DRV_W25Q32_State(); }
/** @brief 查询作业结果。 */
drv_status_t DRV_Storage_Result(void) { return DRV_W25Q32_Result(); }
