/*============================================================================
 * svc_config_journal.h —— Service 层：NOR 配置快照日志，轮换扇区与掉电提交
 * 目标：当前 W25Q32 后端；内核无关，通过 svc_storage 调用抽象 Driver，无 HAL/QP。
 * 一条槽 256 B：16 B 外层头 + svc_param 原记录 + FF 填充 + 尾部 4 B 提交标记。
 * 只在另一个扇区已有有效副本时回收；作业失败不会擦最新副本。
 *===========================================================================*/
#ifndef SVC_CONFIG_JOURNAL_H
#define SVC_CONFIG_JOURNAL_H
#include "svc_param.h"
#include "svc_storage.h"
/* 状态由 AO_Store 静态持有，服务层无可变全局对象；尺寸由介质布局决定。 */
typedef struct {
    SvcStorageState state;
    svc_status_t result;
    drv_status_t driver_result; /* 保留下层原因；对外作业结果使用 Service 状态码。 */
    uint8_t phase;
    bool pending, present, loaded;
    uint16_t scan_slot, length;
    uint8_t used[APP_CFG_CONFIG_JOURNAL_SIZE / APP_CFG_FLASH_SECTOR_SIZE];
    uint32_t latest, generation, counter, target;
    uint8_t page[256], verify[256], record[SVC_PARAM_RECORD_MAX];
} SvcConfigJournal;
typedef struct {
    bool present, loaded;
    uint32_t generation;
    uint32_t address;      /*!< 配置分区相对偏移；无有效记录时 UINT32_MAX */
    uint16_t length;
} SvcConfigJournalInfo;
/** @brief 初始化单实例；作业只由 AO_Store 持有，禁止跨 AO 同时使用。 */
void SVC_ConfigJournal_Init(SvcConfigJournal *j);
/** @brief 异步探测并扫描已提交快照；不擦除，不自动写默认值。 */
svc_status_t SVC_ConfigJournal_LoadStart(SvcConfigJournal *j);
/** @brief 复制待保存快照后发起追加；参数缓冲可立即复用，失败后保留旧有效记录。 */
svc_status_t SVC_ConfigJournal_SaveStart(SvcConfigJournal *j, uint8_t const *record, uint16_t length);
/** @brief 推进一个相位/一笔指令；无忙等，不持 SPI 锁等待擦除。 */
void SVC_ConfigJournal_Process(SvcConfigJournal *j);
/** @brief 查询作业状态/结果。 */
SvcStorageState SVC_ConfigJournal_State(SvcConfigJournal *j);
svc_status_t SVC_ConfigJournal_Result(SvcConfigJournal *j);
/** @brief 返回最新已提交记录的只读缓冲；仅在本服务空闲/完成时使用。 */
uint8_t const *SVC_ConfigJournal_Record(SvcConfigJournal *j);
/** @brief 返回装载/保存信息；地址仅用于诊断，不由业务层直接操作。 */
void SVC_ConfigJournal_Info(SvcConfigJournal *j, SvcConfigJournalInfo *info);
#endif
