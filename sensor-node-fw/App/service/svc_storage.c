/* Service 层：统一分区检查，当前 4 MiB NOR；内核无关，不依赖 HAL/QP。 */
#include "svc_storage.h"
#include "app_config.h"
static SvcStorageRegion const l_regions[] = {
    { APP_CFG_STORAGE_CONFIG_BASE, APP_CFG_STORAGE_CONFIG_SIZE, true },
    { APP_CFG_STORAGE_DATA_BASE, APP_CFG_STORAGE_DATA_SIZE, true },
    { APP_CFG_STORAGE_APP_BASE, APP_CFG_STORAGE_APP_SIZE, false }
};
/** @brief 取固定分区表；非法编号不产生任何介质访问。 */
bool SVC_Storage_Region(SvcStoragePartition p, SvcStorageRegion *r) {
    if ((int)p < 0 || p >= SVC_STORAGE_PART_COUNT || r == (SvcStorageRegion *)0) { return false; }
    *r = l_regions[p]; return true;
}
/** @brief 核对无空洞/无重叠/整扇区布局与容量，防止配置宏静默越界。 */
bool SVC_Storage_LayoutValid(void) {
    DrvStorageGeometry g = DRV_Storage_Geometry();
    uint32_t end = 0U;
    unsigned i;
    if (g.erase_size == 0U || g.page_size == 0U) { return false; }
    for (i = 0U; i < SVC_STORAGE_PART_COUNT; ++i) {
        SvcStorageRegion const *r = &l_regions[i];
        if (r->base != end || r->size == 0U || r->base > g.capacity || r->size > g.capacity - r->base ||
            r->base % g.erase_size != 0U || r->size % g.erase_size != 0U) { return false; }
        end = r->base + r->size;
    }
    return end == g.capacity;
}
/** @brief 检查范围；使用减法避免 offset+length 溢出绕过边界。 */
static bool range(SvcStoragePartition p, uint32_t off, uint32_t len, bool writing, uint32_t *address) {
    SvcStorageRegion r;
    if (!SVC_Storage_Region(p, &r) || len == 0U || off >= r.size || len > r.size - off ||
        (writing && !r.writable)) { return false; }
    *address = r.base + off; return true;
}
/** @brief 分区相对读。 */
svc_status_t SVC_Storage_ReadStart(SvcStoragePartition p, uint32_t off, uint8_t *data, uint32_t len) {
    uint32_t a;
    return range(p, off, len, false, &a) ? SVC_Storage_MapStatus(DRV_Storage_ReadStart(a, data, len)) : SVC_ERR_PARAM;
}
/** @brief 分区相对编程；APP 区固定只读。 */
svc_status_t SVC_Storage_ProgramStart(SvcStoragePartition p, uint32_t off, uint8_t const *data, uint32_t len) {
    uint32_t a;
    return range(p, off, len, true, &a) ? SVC_Storage_MapStatus(DRV_Storage_ProgramStart(a, data, len)) : SVC_ERR_PARAM;
}
/** @brief 分区相对擦除；粒度由 Driver 再次校验。 */
svc_status_t SVC_Storage_EraseStart(SvcStoragePartition p, uint32_t off, uint32_t len) {
    uint32_t a;
    return range(p, off, len, true, &a) ? SVC_Storage_MapStatus(DRV_Storage_EraseStart(a, len)) : SVC_ERR_PARAM;
}

/** @brief 各层保持独立状态码；总线/超时/缺器件映射为设备状态错误。 */
svc_status_t SVC_Storage_MapStatus(drv_status_t st) {
    switch (st) {
        case DRV_OK: return SVC_OK;
        case DRV_ERR_PARAM: return SVC_ERR_PARAM;
        case DRV_ERR_BUSY: return SVC_ERR_BUSY;
        default: return SVC_ERR_STATE;
    }
}
