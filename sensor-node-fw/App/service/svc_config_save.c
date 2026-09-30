/*============================================================================
 * svc_config_save.c —— Service 层：配置快照追加/回收/两阶段提交
 * NOR 后端无关、内核无关，无 HAL/QP。保留最新有效扇区，损坏目标页只跳过不覆盖。
 *===========================================================================*/
#include "svc_config_journal_internal.h"
#include <string.h>

/** @brief 发起保存当前阶段；提交四字节只把 FF 编程为零，不重复写载荷。 */
static svc_status_t issue(ConfigJournal *j) {
    static uint8_t const commit[4] = {0U, 0U, 0U, 0U};
    switch (j->phase) {
        case CFG_ERASE: return SVC_Storage_EraseStart(SVC_STORAGE_CONFIG, j->target, APP_CFG_FLASH_SECTOR_SIZE);
        case CFG_BODY: return SVC_Storage_ProgramStart(SVC_STORAGE_CONFIG, j->target, j->page, CFG_COMMIT_OFFSET);
        case CFG_VERIFY_BODY: case CFG_VERIFY_COMMIT:
            return SVC_Storage_ReadStart(SVC_STORAGE_CONFIG, j->target, j->verify, CFG_SLOT_SIZE);
        case CFG_COMMIT: return SVC_Storage_ProgramStart(SVC_STORAGE_CONFIG, j->target + CFG_COMMIT_OFFSET, commit, sizeof(commit));
        default: return SVC_ERR_PARAM;
    }
}
/** @brief 保存推进；先回读正文再写提交标记，最终回读确认才报告成功。 */
void SVC_ConfigJournal_SaveStep(SvcConfigJournal *j) {
    unsigned sector = (unsigned)((j->target - APP_CFG_CONFIG_JOURNAL_OFFSET) / APP_CFG_FLASH_SECTOR_SIZE);
    svc_status_t st;
    if (!j->pending) {
        /* 防御：即使状态损坏也禁止回收最新有效快照所在的扇区。 */
        if (j->phase == CFG_ERASE && j->loaded &&
            sector == (j->latest - APP_CFG_CONFIG_JOURNAL_OFFSET) / APP_CFG_FLASH_SECTOR_SIZE) {
            SVC_ConfigJournal_Finish(j, SVC_ERR_PARAM); return;
        }
        st = issue(j);
        if (st == SVC_OK) {
            j->pending = true;
            if (j->phase == CFG_BODY) {
                j->used[sector] = (uint8_t)((j->target % APP_CFG_FLASH_SECTOR_SIZE) / CFG_SLOT_SIZE + 1U);
            }
        } else if (st != SVC_ERR_BUSY) { SVC_ConfigJournal_Finish(j, st); }
        return;
    }
    if (DRV_Storage_State() == DRV_STORAGE_BUSY) { return; }
    j->pending = false;
    if (DRV_Storage_Result() != DRV_OK) {
        j->driver_result = DRV_Storage_Result(); SVC_ConfigJournal_Finish(j, SVC_Storage_MapStatus(DRV_Storage_Result())); return; }
    switch (j->phase) {
        case CFG_ERASE:
            j->used[sector] = 0U; j->phase = CFG_BODY; break;
        case CFG_BODY: j->phase = CFG_VERIFY_BODY; break;
        case CFG_VERIFY_BODY:
            if (memcmp(j->page, j->verify, CFG_SLOT_SIZE) != 0) { SVC_ConfigJournal_Finish(j, SVC_ERR_STATE); }
            else { j->phase = CFG_COMMIT; }
            break;
        case CFG_COMMIT: j->phase = CFG_VERIFY_COMMIT; break;
        case CFG_VERIFY_COMMIT: {
            uint32_t generation; uint16_t length;
            if (!SVC_ConfigJournal_Decode(j->verify, &generation, &length) || generation != j->counter ||
                memcmp(j->page, j->verify, CFG_COMMIT_OFFSET) != 0) {
                SVC_ConfigJournal_Finish(j, SVC_ERR_STATE); break;
            }
            j->latest = j->target; j->generation = generation; j->length = length; j->loaded = true;
            memcpy(j->record, j->verify + CFG_HEADER_SIZE, length);
            j->phase = CFG_READY; SVC_ConfigJournal_Finish(j, SVC_OK); break;
        }
        default: SVC_ConfigJournal_Finish(j, SVC_ERR_PARAM); break;
    }
}
