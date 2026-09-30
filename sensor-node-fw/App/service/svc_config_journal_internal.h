/* Service 层内部：配置日志共享上下文；纯 C，无 HAL/QP，当前 256 B 记录槽。 */
#ifndef SVC_CONFIG_JOURNAL_INTERNAL_H
#define SVC_CONFIG_JOURNAL_INTERNAL_H
#include "svc_config_journal.h"
#include "svc_storage.h"
#define CFG_SLOT_SIZE (256U)
#define CFG_HEADER_SIZE (16U)
#define CFG_COMMIT_OFFSET (252U)
#define CFG_SECTOR_COUNT (APP_CFG_CONFIG_JOURNAL_SIZE / APP_CFG_FLASH_SECTOR_SIZE)
#define CFG_SLOTS_PER_SECTOR (APP_CFG_FLASH_SECTOR_SIZE / CFG_SLOT_SIZE)
typedef enum { CFG_PROBE, CFG_SCAN, CFG_READY, CFG_ERASE, CFG_BODY, CFG_VERIFY_BODY, CFG_COMMIT, CFG_VERIFY_COMMIT } ConfigPhase;
typedef SvcConfigJournal ConfigJournal;
void SVC_ConfigJournal_Finish(SvcConfigJournal *j, svc_status_t result);
void SVC_ConfigJournal_ScanStep(SvcConfigJournal *j);
void SVC_ConfigJournal_SaveStep(SvcConfigJournal *j);
bool SVC_ConfigJournal_Decode(uint8_t const *page, uint32_t *generation, uint16_t *length);
void SVC_ConfigJournal_Build(uint8_t *page, uint32_t generation, uint8_t const *record, uint16_t length);
#endif
