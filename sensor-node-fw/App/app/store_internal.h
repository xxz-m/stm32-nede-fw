/* App 层内部：参数 AO 上下文，STM32F103RCT6 / QP/C QV；不依赖具体存储器。 */
#ifndef STORE_INTERNAL_H
#define STORE_INTERNAL_H
#include "store.h"
#include "svc_config_journal.h"
typedef enum { STORE_DISABLED, STORE_LOADING, STORE_READY, STORE_SAVING } StorePhase;
typedef struct {
    QActive super;
    QTimeEvt tick;
    SvcParam param;
    SvcConfigJournal journal;
    StorePhase phase;
    bool present, resolved, blocked;
    uint8_t attempts, next_seq;
    uint16_t next_version;
    uint32_t saved, failed, edit_seq, save_edit_seq;
} Store;
extern Store STORE_instance;
void App_Store_PostEdit(void);
bool App_Store_ScanRequest(void);
bool App_Store_ScanBusy(void);
void App_Store_ScanProcess(void);
#endif
