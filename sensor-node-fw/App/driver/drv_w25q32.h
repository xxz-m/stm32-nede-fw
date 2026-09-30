/* Driver 层：W25Q32JV 后端内部接口；硬件命令在此后端，内核无关，不暴露 HAL。 */
#ifndef DRV_W25Q32_H
#define DRV_W25Q32_H
#include "drv_storage.h"
#include "app_config.h"
typedef enum { W25_PROBE, W25_READ, W25_PROGRAM, W25_ERASE } W25Operation;
void DRV_W25Q32_Init(void);
void DRV_W25Q32_Process(void);
drv_status_t DRV_W25Q32_Start(W25Operation op, uint32_t addr, uint8_t *read,
                             uint8_t const *write, uint32_t length);
DrvStorageState DRV_W25Q32_State(void);
drv_status_t DRV_W25Q32_Result(void);
/* 内部事务工具：Start 为 BUSY 时不改缓冲；Result 完成后才可构造下一命令。 */
drv_status_t DRV_W25Q32_IoStart(uint8_t command, uint32_t address, bool addressed,
                               uint8_t const *data, uint16_t length);
bool DRV_W25Q32_IoResult(drv_status_t *result);
uint8_t const *DRV_W25Q32_IoData(void);
#endif
