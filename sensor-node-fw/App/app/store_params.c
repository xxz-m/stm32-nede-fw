/* App 层：参数影子接口与 AT+CFG? 状态，STM32F103RCT6 / QV；不依赖芯片驱动。 */
#include "store_internal.h"
/** @brief 只读参数 RAM 影子，地址稳定，供 BLE/4G 使用。 */
char const *App_Store_Value(uint8_t id) { return SVC_Param_Value(&STORE_instance.param, id); }
/** @brief 成功或失败均结束启动等待，不让介质故障阻塞整个应用。 */
bool App_Store_ConfigResolved(void) { return STORE_instance.resolved; }
/** @brief 未落盘的改动、加载、保存、诊断都阻止安全复位。 */
bool App_Store_IsIdle(void) {
    Store const *me = &STORE_instance;
    if (me->phase == STORE_DISABLED) { return true; }
    return me->resolved && !me->param.dirty && me->phase == STORE_READY && !App_Store_ScanBusy();
}
/** @brief 修改影子并通知 AO；保存途中编辑以 edit_seq 区分，失败后下一次编辑允许重试。 */
svc_status_t App_Store_SetName(char const *name, char const *value) {
    Store *me = &STORE_instance;
    int16_t id;
    svc_status_t st;
    if (!me->resolved) { return SVC_ERR_STATE; }
    id = SVC_Param_Find(name);
    if (id < 0) { return SVC_ERR_PARAM; }
    st = SVC_Param_SetValue(&me->param, (uint8_t)id, value);
    if (st != SVC_OK) { return st; }
    if (me->phase != STORE_DISABLED) {
        ++me->edit_seq; me->param.dirty = true; me->blocked = false; me->attempts = 0U;
        App_Store_PostEdit();
    }
    return SVC_OK;
}
/** @brief 保持 AT&F 语义：恢复全部参数（包括 SN），保留版本计数，异步保存。 */
bool App_Store_ResetDefaults(void) {
    Store *me = &STORE_instance;
    uint16_t ver = me->param.config_version;
    bool loaded = me->param.from_media;
    if (!me->resolved) { return false; }
    SVC_Param_SetDefaults(&me->param);
    me->param.config_version = ver; me->param.from_media = loaded;
    if (me->phase != STORE_DISABLED) {
        ++me->edit_seq; me->param.dirty = true; me->blocked = false; me->attempts = 0U;
        App_Store_PostEdit();
    }
    return true;
}
/** @brief 保留 I2C 扫描命令，诊断实际 I2C 总线，不把它改成 Flash 命令。 */
bool App_Store_RequestScan(void) {
    if (!STORE_instance.resolved || STORE_instance.phase == STORE_DISABLED) { return false; }
    if (!App_Store_ScanRequest()) { return false; }
    App_Store_PostEdit(); return true;
}
/** @brief 原八字段状态快照；dirty 在失败时保持真，避免宣称已经落盘。 */
void App_Store_Info(AppStoreInfo *out) {
    Store const *me = &STORE_instance;
    if (out == (AppStoreInfo *)0) { return; }
    out->present = me->present; out->resolved = me->resolved; out->loaded = me->param.from_media;
    out->dirty = me->param.dirty; out->slot_seq = me->param.slot_seq; out->config_version = me->param.config_version;
    out->saved = me->saved; out->failed = me->failed;
}
