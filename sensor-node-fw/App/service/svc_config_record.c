/*============================================================================
 * svc_config_record.c —— Service 层：配置快照外层格式与校验
 * 目标介质：NOR 通用日志；内核无关。源于本工程，无第三方代码。
 * NOTE (提交)：只有尾部四字节全为零且头/载荷 CRC 均正确才承认有效记录。
 *===========================================================================*/
#include "svc_config_journal_internal.h"
#include <string.h>

/** @brief 解码小端整数；不依赖结构体对齐或编译器填充。 */
static uint32_t get32(uint8_t const *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
/** @brief 显式写小端整数，避免把结构体布局写进介质。 */
static void put32(uint8_t *p, uint32_t v) {
    unsigned i; for (i = 0U; i < 4U; ++i) { p[i] = (uint8_t)(v >> (i * 8U)); }
}
/** @brief 读取小端 16 位数。 */
static uint16_t get16(uint8_t const *p) { return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8); }
/** @brief 写入小端 16 位数。 */
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

/** @brief 构造未提交页；内层 ATP1 记录保持兼容，外层 CFJ1 使用 32 位序号。 */
void SVC_ConfigJournal_Build(uint8_t *page, uint32_t generation, uint8_t const *record, uint16_t length) {
    memset(page, 0xFF, CFG_SLOT_SIZE);
    memcpy(page, "CFJ1", 4U);
    put32(page + 4U, generation);
    put16(page + 8U, length);
    put16(page + 10U, 1U); /* 外层格式版本 */
    put16(page + 12U, SVC_Frame_Crc16(record, length));
    put16(page + 14U, SVC_Frame_Crc16(page, 14U));
    memcpy(page + CFG_HEADER_SIZE, record, length);
}

/** @brief 校验完整提交、两层 CRC 和参数格式；损坏新页不能压过旧有效记录。 */
bool SVC_ConfigJournal_Decode(uint8_t const *page, uint32_t *generation, uint16_t *length) {
    SvcParam parsed;
    uint16_t len = get16(page + 8U);
    if (memcmp(page, "CFJ1", 4U) != 0 || get16(page + 10U) != 1U ||
        get32(page + CFG_COMMIT_OFFSET) != 0U || len > SVC_PARAM_RECORD_MAX ||
        len < SVC_PARAM_HDR_SIZE + SVC_PARAM_CRC_SIZE || len > CFG_COMMIT_OFFSET - CFG_HEADER_SIZE ||
        get16(page + 14U) != SVC_Frame_Crc16(page, 14U) ||
        get16(page + 12U) != SVC_Frame_Crc16(page + CFG_HEADER_SIZE, len)) { return false; }
    SVC_Param_SetDefaults(&parsed);
    if (SVC_Param_Decode(&parsed, page + CFG_HEADER_SIZE, len) != SVC_OK) { return false; }
    *generation = get32(page + 4U); *length = len;
    return true;
}
