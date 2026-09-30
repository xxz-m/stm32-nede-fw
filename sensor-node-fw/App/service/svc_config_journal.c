/* Service 层：配置日志装载与作业入口，NOR 后端无关，内核无关，不使用 HAL/QP。 */
#include "svc_config_journal_internal.h"
#include <string.h>


/** @brief 结束一个作业；失败由调用方报告，不触发无限自动重试。 */
void SVC_ConfigJournal_Finish(SvcConfigJournal *j, svc_status_t result) {
    j->result = result;
    j->pending = false;
    j->state = result == SVC_OK ? SVC_STORAGE_DONE : SVC_STORAGE_FAILED;
}
/** @brief 清上下文，不访问总线；保留页日志序号与 ATP1 序号的独立性。 */
void SVC_ConfigJournal_Init(SvcConfigJournal *j) {
    if (j == (SvcConfigJournal *)0) { return; }
    memset(j, 0, sizeof(*j));
    j->latest = UINT32_MAX;
    DRV_Storage_Init();
}
/** @brief 启动只读装载，拒绝越界、非整扇区和单扇区日志布局。 */
svc_status_t SVC_ConfigJournal_LoadStart(SvcConfigJournal *j) {
    if (j == (SvcConfigJournal *)0) { return SVC_ERR_PARAM; }
    if (j->state == SVC_STORAGE_BUSY) { return SVC_ERR_BUSY; }
    if (!SVC_Storage_LayoutValid() || CFG_SECTOR_COUNT < 2U ||
        APP_CFG_CONFIG_JOURNAL_OFFSET % APP_CFG_FLASH_SECTOR_SIZE != 0U ||
        APP_CFG_CONFIG_JOURNAL_SIZE % APP_CFG_FLASH_SECTOR_SIZE != 0U ||
        APP_CFG_CONFIG_JOURNAL_OFFSET > APP_CFG_STORAGE_CONFIG_SIZE ||
        APP_CFG_CONFIG_JOURNAL_SIZE > APP_CFG_STORAGE_CONFIG_SIZE - APP_CFG_CONFIG_JOURNAL_OFFSET ||
        CFG_HEADER_SIZE + SVC_PARAM_RECORD_MAX > CFG_COMMIT_OFFSET ||
        APP_CFG_FLASH_PAGE_SIZE != CFG_SLOT_SIZE) { return SVC_ERR_PARAM; }
    memset(j->used, 0, sizeof(j->used));
    j->latest = UINT32_MAX; j->loaded = j->present = false;
    j->scan_slot = 0U; j->length = 0U; j->counter = j->generation = 0U;
    j->pending = false; j->phase = CFG_PROBE; j->state = SVC_STORAGE_BUSY;
    return SVC_OK;
}
/** @brief 追加一条快照；尾部空间不足时切到下一扇区，绝不擦最新记录所在扇区。 */
svc_status_t SVC_ConfigJournal_SaveStart(SvcConfigJournal *j, uint8_t const *record, uint16_t length) {
    unsigned sector;
    if (j == (SvcConfigJournal *)0) { return SVC_ERR_PARAM; }
    if (j->state == SVC_STORAGE_BUSY) { return SVC_ERR_BUSY; }
    if (!j->present || record == (uint8_t const *)0 || length > SVC_PARAM_RECORD_MAX ||
        length < SVC_PARAM_HDR_SIZE + SVC_PARAM_CRC_SIZE) { return SVC_ERR_PARAM; }
    sector = j->loaded ? (unsigned)((j->latest - APP_CFG_CONFIG_JOURNAL_OFFSET) / APP_CFG_FLASH_SECTOR_SIZE) : 0U;
    if (j->used[sector] >= CFG_SLOTS_PER_SECTOR) { sector = (sector + 1U) % CFG_SECTOR_COUNT; }
    j->target = APP_CFG_CONFIG_JOURNAL_OFFSET + sector * APP_CFG_FLASH_SECTOR_SIZE;
    j->phase = j->used[sector] >= CFG_SLOTS_PER_SECTOR ? CFG_ERASE : CFG_BODY;
    if (j->phase == CFG_BODY) { j->target += j->used[sector] * CFG_SLOT_SIZE; }
    ++j->counter;
    SVC_ConfigJournal_Build(j->page, j->counter, record, length);
    j->state = SVC_STORAGE_BUSY; j->pending = false;
    return SVC_OK;
}
/** @brief 推进底层后处理一个相位；作业完成才可进入下一阶段。 */
void SVC_ConfigJournal_Process(SvcConfigJournal *j) {
    if (j == (SvcConfigJournal *)0) { return; }
    if (j->state != SVC_STORAGE_BUSY) { return; }
    DRV_Storage_Process();
    if (j->phase == CFG_PROBE || j->phase == CFG_SCAN) {
        SVC_ConfigJournal_ScanStep(j);
    } else { SVC_ConfigJournal_SaveStep(j); }
}
/** @brief 状态查询。 */
SvcStorageState SVC_ConfigJournal_State(SvcConfigJournal *j) { return j != (SvcConfigJournal *)0 ? j->state : SVC_STORAGE_FAILED; }
/** @brief 最近作业结果查询。 */
svc_status_t SVC_ConfigJournal_Result(SvcConfigJournal *j) { return j != (SvcConfigJournal *)0 ? j->result : SVC_ERR_PARAM; }
/** @brief 最新已提交记录，不返回尚在保存的影子。 */
uint8_t const *SVC_ConfigJournal_Record(SvcConfigJournal *j) { return j != (SvcConfigJournal *)0 ? j->record : (uint8_t const *)0; }
/** @brief 返回最新成功记录的元数据。 */
void SVC_ConfigJournal_Info(SvcConfigJournal *j, SvcConfigJournalInfo *info) {
    if (j == (SvcConfigJournal *)0 || info == (SvcConfigJournalInfo *)0) { return; }
    info->present = j->present; info->loaded = j->loaded; info->generation = j->generation;
    info->address = j->latest; info->length = j->length;
}
