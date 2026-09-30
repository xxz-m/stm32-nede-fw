/* Service 层：配置日志只读扫描；NOR 后端无关/内核无关，无 HAL/QP。 */
#include "svc_config_journal_internal.h"
#include <string.h>

/** @brief 判断整个槽是否空白；撕裂写入占用过的槽即使 CRC 错也不再覆盖编程。 */
static bool blank(uint8_t const *page) {
    unsigned i; for (i = 0U; i < CFG_SLOT_SIZE; ++i) { if (page[i] != 0xFFU) { return false; } }
    return true;
}
/** @brief 序号回绕安全比较；存活记录跨度远小于 2^31。 */
static bool newer(uint32_t a, uint32_t b) { uint32_t d = a - b; return d != 0U && d < 0x80000000UL; }

/** @brief 一拍探测/读取一个槽；全页 CRC 校验后才参与最新记录选择。 */
void SVC_ConfigJournal_ScanStep(SvcConfigJournal *j) {
    svc_status_t st;
    uint32_t address = APP_CFG_CONFIG_JOURNAL_OFFSET + (uint32_t)j->scan_slot * CFG_SLOT_SIZE;
    if (!j->pending) {
        st = j->phase == CFG_PROBE ? SVC_Storage_MapStatus(DRV_Storage_ProbeStart()) :
             SVC_Storage_ReadStart(SVC_STORAGE_CONFIG, address, j->verify, CFG_SLOT_SIZE);
        if (st == SVC_OK) { j->pending = true; }
        else if (st != SVC_ERR_BUSY) { SVC_ConfigJournal_Finish(j, st); }
        return;
    }
    if (DRV_Storage_State() == DRV_STORAGE_BUSY) { return; }
    j->pending = false;
    if (DRV_Storage_Result() != DRV_OK) {
        j->driver_result = DRV_Storage_Result();
        /* 读失败不当成空白，避免恢复默认值后擦除可能仍然有效的配置。 */
        j->present = false;
        SVC_ConfigJournal_Finish(j, SVC_Storage_MapStatus(DRV_Storage_Result())); return;
    }
    if (j->phase == CFG_PROBE) { j->present = true; j->phase = CFG_SCAN; return; }
    if (!blank(j->verify)) {
        unsigned sector = j->scan_slot / CFG_SLOTS_PER_SECTOR;
        j->used[sector] = (uint8_t)(j->scan_slot % CFG_SLOTS_PER_SECTOR + 1U);
    }
    {
        uint32_t generation; uint16_t length;
        if (SVC_ConfigJournal_Decode(j->verify, &generation, &length) &&
            (!j->loaded || newer(generation, j->generation))) {
            j->loaded = true; j->generation = j->counter = generation;
            j->latest = address; j->length = length;
            memcpy(j->record, j->verify + CFG_HEADER_SIZE, length);
        }
    }
    ++j->scan_slot;
    if (j->scan_slot == CFG_SECTOR_COUNT * CFG_SLOTS_PER_SECTOR) {
        j->phase = CFG_READY; SVC_ConfigJournal_Finish(j, SVC_OK);
    }
}
