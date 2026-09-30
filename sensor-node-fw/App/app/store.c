/*============================================================================
 * store.c —— App 层：AO_Store，参数 RAM 影子与 NOR 配置日志编排
 * 目标平台：STM32F103RCT6；所属内核：QP/C 6.9.1 QV。
 * 只调用 svc_param/svc_config_journal 与 BSP；不依赖具体存储芯片。
 * NOTE (失败)：保存失败保留 dirty 并停止无限重试；下一次编辑可重新尝试。
 *===========================================================================*/
#include "store_internal.h"
#include "app_prio.h"
#include "app_signals.h"
#include "bsp.h"
#include "svc_log.h"
#include <string.h>
Q_DEFINE_THIS_MODULE("store")
void Q_onAssert(char const * const module, int loc);
Store STORE_instance;
QActive * const AO_Store = &STORE_instance.super;
static QEvt const *l_queue[8];
#define STORE_TICKS ((APP_CFG_STORE_PROCESS_PERIOD_MS * BSP_TICKS_PER_SEC) / 1000U)
/** @brief 一次成功或失败的装载都结束启动等待；读错误时禁止覆盖介质。 */
static void loadFinished(Store *me) {
    SvcConfigJournalInfo info;
    SVC_ConfigJournal_Info(&me->journal, &info);
    me->present = info.present;
    if (SVC_ConfigJournal_Result(&me->journal) == SVC_OK) {
        if (info.loaded && SVC_Param_Decode(&me->param, SVC_ConfigJournal_Record(&me->journal), info.length) == SVC_OK) {
            me->param.from_media = true;
            LOG_I("store: loaded ver=%u seq=%u gen=%lu offset=0x%04lX",
                  (unsigned)me->param.config_version, (unsigned)me->param.slot_seq,
                  (unsigned long)info.generation, (unsigned long)info.address);
        } else { LOG_I("store: blank flash, using built-in defaults"); }
    } else {
        ++me->failed; me->blocked = true;
        LOG_E("store: flash load failed (%u), using defaults; media writes disabled",
              (unsigned)SVC_ConfigJournal_Result(&me->journal));
    }
    me->param.dirty = false; me->resolved = true; me->phase = STORE_READY;
}
/** @brief 对当前影子编码一次；服务复制记录，后续编辑不改变正在写入的快照。 */
static void beginSave(Store *me) {
    uint8_t record[SVC_PARAM_RECORD_MAX];
    size_t length;
    svc_status_t st;
    me->next_version = (uint16_t)(me->param.config_version + 1U);
    me->next_seq = (uint8_t)(me->param.slot_seq + 1U);
    length = SVC_Param_Encode(&me->param, me->next_version, me->next_seq, record, sizeof(record));
    if (length == 0U) { me->blocked = true; ++me->failed; return; }
    st = SVC_ConfigJournal_SaveStart(&me->journal, record, (uint16_t)length);
    if (st == SVC_OK) { me->save_edit_seq = me->edit_seq; me->phase = STORE_SAVING; }
    else if (st != SVC_ERR_BUSY) {
        ++me->failed; me->blocked = true;
        LOG_E("store: save rejected (%u), change not persisted", (unsigned)st);
    }
}
/** @brief 只有提交和最终回读均正确才更新版本及清 dirty。 */
static void saveFinished(Store *me) {
    if (SVC_ConfigJournal_Result(&me->journal) == SVC_OK) {
        SvcConfigJournalInfo info;
        SVC_ConfigJournal_Info(&me->journal, &info);
        me->param.config_version = me->next_version; me->param.slot_seq = me->next_seq;
        me->param.from_media = true; me->param.dirty = me->edit_seq != me->save_edit_seq;
        ++me->saved; me->attempts = 0U;
        LOG_I("store: saved ver=%u seq=%u gen=%lu offset=0x%04lX %u B",
              (unsigned)me->next_version, (unsigned)me->next_seq,
              (unsigned long)info.generation, (unsigned long)info.address, (unsigned)info.length);
    } else {
        ++me->failed;
        if (++me->attempts > APP_CFG_STORE_RETRY) { me->blocked = true; }
        LOG_E("store: save failed (%u), dirty retained, retry=%u blocked=%u",
              (unsigned)SVC_ConfigJournal_Result(&me->journal), (unsigned)me->attempts, me->blocked ? 1U : 0U);
    }
    me->phase = STORE_READY;
}
/** @brief 推进日志或 I2C 诊断；所有存储作业只由本 AO 持有。 */
static void process(Store *me) {
    if (me->phase == STORE_DISABLED) { return; }
    if (me->phase == STORE_LOADING || me->phase == STORE_SAVING) {
        SVC_ConfigJournal_Process(&me->journal);
        if (SVC_ConfigJournal_State(&me->journal) == SVC_STORAGE_BUSY) { return; }
        if (me->phase == STORE_LOADING) { loadFinished(me); } else { saveFinished(me); }
        return;
    }
    if (me->param.dirty && !me->blocked && me->present) { beginSave(me); return; }
    App_Store_ScanProcess(); /* 保留 AT+I2C? 诊断，独立于 Flash 参数介质。 */
}
/** @brief 单状态 AO，周期推进 DMA 结果和芯片 BUSY，禁止原地等待。 */
static QState active(Store * const me, QEvt const * const e) {
    switch (e->sig) {
        case Q_ENTRY_SIG: QTimeEvt_armX(&me->tick, STORE_TICKS, STORE_TICKS); return Q_HANDLED();
        case STORE_TICK_SIG: case STORE_BUS_SIG: case STORE_CMD_SIG: process(me); return Q_HANDLED();
        default: return Q_SUPER(&QHsm_top);
    }
}
/** @brief 初始转换只进入 Active。 */
static QState initial(Store * const me, void const * const par) { (void)me; (void)par; return Q_TRAN(&active); }
/** @brief 构造影子和日志作业；必须先于 BLE/4G 构造，不访问总线。 */
void Store_ctor(void) {
    Store *me = &STORE_instance;
    memset(me, 0, sizeof(*me));
    QActive_ctor(&me->super, Q_STATE_CAST(&initial));
    QTimeEvt_ctorX(&me->tick, &me->super, STORE_TICK_SIG, 0U);
    SVC_Param_SetDefaults(&me->param); SVC_ConfigJournal_Init(&me->journal);
#if APP_CFG_STORE_ENABLE == 0
    me->phase = STORE_DISABLED; me->resolved = true;
#else
    me->phase = STORE_LOADING;
    if (SVC_ConfigJournal_LoadStart(&me->journal) != SVC_OK) {
        me->phase = STORE_READY; me->resolved = true; me->blocked = true; ++me->failed;
        LOG_E("store: invalid partition layout, media writes disabled");
    }
#endif
}
/** @brief 启动 AO；静态队列与 QV 单栈，不增加活动对象。 */
void Store_start(void) {
    QACTIVE_START(AO_Store, APP_STORE_PRIO, l_queue, Q_DIM(l_queue), (void *)0, 0U, (QEvt *)0);
}
/** @brief I2C 兼容完成通知，只投递事件；Flash DMA 完成不经过这个入口。 */
void Store_i2cSink(void) {
    static QEvt const evt = { (QSignal)STORE_BUS_SIG, 0U, 0U };
    (void)QACTIVE_POST_X(AO_Store, &evt, 0U, 0U);
}
/** @brief 主上下文编辑通知，调用者不等待落盘。 */
void App_Store_PostEdit(void) {
    static QEvt const evt = { (QSignal)STORE_CMD_SIG, 0U, 0U };
    (void)QACTIVE_POST_X(AO_Store, &evt, 0U, 0U);
}
