/* Service 层：存储分区边界与权限，芯片无关；STM32F103/QV 调用方，禁止 HAL/QP 依赖。 */
#ifndef SVC_STORAGE_H
#define SVC_STORAGE_H
#include "drv_storage.h"
#include "svc_frame.h"
typedef enum { SVC_STORAGE_IDLE = 0, SVC_STORAGE_BUSY, SVC_STORAGE_DONE, SVC_STORAGE_FAILED } SvcStorageState;
/** @brief 映射下层状态，原始作业原因仍可在 Driver 查询。 */
svc_status_t SVC_Storage_MapStatus(drv_status_t status);
typedef enum { SVC_STORAGE_CONFIG = 0, SVC_STORAGE_DATA, SVC_STORAGE_APP, SVC_STORAGE_PART_COUNT } SvcStoragePartition;
typedef struct { uint32_t base, size; bool writable; } SvcStorageRegion;
/** @brief 获取分区；APP 仅预留，禁止编程和擦除。 */
bool SVC_Storage_Region(SvcStoragePartition part, SvcStorageRegion *region);
/** @brief 检查分区布局与后端容量/擦除粒度；不访问总线。 */
bool SVC_Storage_LayoutValid(void);
/** @brief 从分区相对偏移读，拒绝跨区，忙时由调用方稍后重试。 */
svc_status_t SVC_Storage_ReadStart(SvcStoragePartition part, uint32_t offset, uint8_t *data, uint32_t length);
/** @brief 从分区相对偏移编程，拒绝跨区及 APP 区。 */
svc_status_t SVC_Storage_ProgramStart(SvcStoragePartition part, uint32_t offset, uint8_t const *data, uint32_t length);
/** @brief 擦除分区整扇区，拒绝不对齐、跨区及 APP 区。 */
svc_status_t SVC_Storage_EraseStart(SvcStoragePartition part, uint32_t offset, uint32_t length);
#endif
