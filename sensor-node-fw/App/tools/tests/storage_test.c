/*============================================================================
 * 主机验证：编译实际 Driver/Service，用 NOR 命令模拟器检查跨页、分区与掉电提交。
 * 不用于固件，无 QP/HAL；模型强制 NOR 1->0、WEL、BUSY 与 256 B 页边界。
 *===========================================================================*/
#include "bsp.h"
#include "drv_storage.h"
#include "svc_config_journal_internal.h"
#include "store_internal.h"
#include "app_signals.h"
#include "svc_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static SvcConfigJournal SVC_configJournal;
static uint8_t media[APP_CFG_FLASH_CAPACITY], baseline[APP_CFG_FLASH_CAPACITY];
static uint32_t now, mutations, programs, erases, polls;
static int cut_after = -1, chip_busy;
static bool cut, wel, owned, ready, sensor_owned, io_error, never_ready;
static uint8_t id_type = 0x40U;
static bsp_status_t transaction_result;
/* 测试不打印参数与凭据；日志接口仅作为链接替身。 */
void SVC_Log_Printf(svc_log_lvl_t level, char const *format, ...) { (void)level; (void)format; }
void SVC_Log_Write(char const *text) { (void)text; }
bsp_status_t BSP_i2cProbe(uint8_t address, uint32_t timeout) { (void)address; (void)timeout; return BSP_ERR_IO; }

/** @brief 模拟毫秒时基，每次推进由测试调用者加 10 ms，允许回绕。 */
uint32_t BSP_uptimeMs(void) { return now; }
/** @brief 模拟所有权，包含未取结果与传感器占用。 */
bool BSP_spiIsBusy(void) { return owned || sensor_owned; }
/** @brief 执行一条 NOR 指令，事务结果到下一次 Process 才可取。 */
bsp_status_t BSP_spiTransferStart(BspSpiDevice dev, uint8_t const *tx, uint8_t *rx, uint16_t n) {
    uint8_t cmd = tx[0];
    uint32_t addr = n >= 4U ? (uint32_t)tx[1] << 16 | (uint32_t)tx[2] << 8 | tx[3] : 0U;
    unsigned i;
    if (BSP_spiIsBusy()) { return BSP_ERR_BUSY; }
    CHECK(dev == BSP_SPI_FLASH && n > 0U && n <= APP_CFG_SPI_TRANSFER_MAX);
    owned = true; ready = false; transaction_result = io_error ? BSP_ERR_IO : BSP_OK;
    memset(rx, 0xFF, n);
    if (io_error) { return BSP_OK; }
    if (cmd == 0x05U) {
        ++polls; rx[1] = (uint8_t)((chip_busy || never_ready ? 1U : 0U) | (wel ? 2U : 0U));
        if (chip_busy) { --chip_busy; }
    } else {
        CHECK(!chip_busy && !never_ready);
        if (cmd == 0x9FU) { CHECK(n == 4U); rx[1] = 0xEFU; rx[2] = id_type; rx[3] = 0x16U; }
        else if (cmd == 0x06U) { CHECK(n == 1U); wel = true; }
        else if (cmd == 0x03U) { CHECK(addr + n - 4U <= sizeof(media)); memcpy(rx + 4U, media + addr, n - 4U); }
        else if (cmd == 0x02U || cmd == 0x20U) {
            unsigned count = cmd == 0x20U ? APP_CFG_FLASH_SECTOR_SIZE : n - 4U;
            CHECK(wel && addr + count <= sizeof(media));
            if (cmd == 0x02U) { CHECK((addr % 256U) + count <= 256U); ++programs; }
            else { CHECK(addr % 4096U == 0U); ++erases; }
            wel = false;
            for (i = 0U; i < count; ++i) {
                if (cut_after == 0) { cut = true; break; }
                if (cmd == 0x02U) { media[addr+i] &= tx[4U+i]; } else { media[addr+i] = 0xFFU; }
                ++mutations; if (cut_after > 0) { --cut_after; }
            }
            chip_busy = 2;
        } else { CHECK(false); }
    }
    return BSP_OK;
}
/** @brief 模拟 DMA 完成，只使结果可取，所有权由原设备取走。 */
void BSP_spiProcess(void) { if (owned) { ready = true; } }
/** @brief 非所有者不得取走 Flash 事务结果。 */
bool BSP_spiResult(BspSpiDevice dev, bsp_status_t *result) {
    if (!owned || !ready || dev != BSP_SPI_FLASH) { return false; }
    *result = transaction_result; owned = false; return true;
}
/** @brief 模拟 MCU 重启，保留 NOR 字节，不保留进程内状态。 */
static void reboot(void) {
    owned = ready = sensor_owned = wel = cut = io_error = never_ready = false;
    chip_busy = 0; cut_after = -1; SVC_ConfigJournal_Init(&SVC_configJournal);
}
/** @brief 推进实际服务直到完成或模拟掉电。 */
static void runJournal(void) {
    unsigned budget = 20000U;
    while (SVC_ConfigJournal_State(&SVC_configJournal) == SVC_STORAGE_BUSY && !cut && budget--) {
        now += 10U; SVC_ConfigJournal_Process(&SVC_configJournal);
    }
    CHECK(cut || SVC_ConfigJournal_State(&SVC_configJournal) != SVC_STORAGE_BUSY);
}
/** @brief 推进实际驱动；退出时绝不遗留 SPI 所有权。 */
static void runDriver(void) {
    unsigned budget = 20000U;
    while (DRV_Storage_State() == DRV_STORAGE_BUSY && budget--) { now += 10U; DRV_Storage_Process(); }
    CHECK(DRV_Storage_State() != DRV_STORAGE_BUSY && !owned);
}
/** @brief 读取日志并检查最终记录里的业务参数。 */
static unsigned loadValue(void) {
    SvcParam p; SvcConfigJournalInfo info;
    CHECK(SVC_ConfigJournal_LoadStart(&SVC_configJournal) == SVC_OK); runJournal();
    CHECK(SVC_ConfigJournal_Result(&SVC_configJournal) == SVC_OK);
    SVC_ConfigJournal_Info(&SVC_configJournal, &info); CHECK(info.present);
    SVC_Param_SetDefaults(&p);
    if (info.loaded) { CHECK(SVC_Param_Decode(&p, SVC_ConfigJournal_Record(&SVC_configJournal), info.length) == SVC_OK); }
    return (unsigned)atoi(SVC_Param_Value(&p, SVC_PARAM_TRANSINTVL));
}
/** @brief 构造合法 ATP1 参数快照并启动实际追加流程。 */
static void startSave(unsigned value) {
    SvcParam p; uint8_t record[SVC_PARAM_RECORD_MAX]; char text[8]; size_t n;
    SVC_Param_SetDefaults(&p); snprintf(text, sizeof(text), "%u", value);
    CHECK(SVC_Param_SetValue(&p, SVC_PARAM_TRANSINTVL, text) == SVC_OK);
    n = SVC_Param_Encode(&p, 1U, 1U, record, sizeof(record)); CHECK(n > 0U);
    CHECK(SVC_ConfigJournal_SaveStart(&SVC_configJournal, record, (uint16_t)n) == SVC_OK);
}

/** @brief 推进实际 AO 状态处理；框架调度由固件 L2 检查，不由替身证明。 */
static void actorTick(void) {
    QEvt const tick = {STORE_TICK_SIG,0U,0U}; now += 10U;
    (void)AO_Store->handler(AO_Store, &tick);
}
/** @brief 检查保存途中再次编辑、失败不清 dirty、禁用时恢复默认值。 */
static void actorTests(void) {
    unsigned budget;
    memset(media,0xFF,APP_CFG_CONFIG_JOURNAL_SIZE); reboot(); Store_ctor(); Store_start();
    CHECK(App_Store_SetName("transintvl","2") == SVC_ERR_STATE);
    for (budget=0U;budget<2000U && !App_Store_ConfigResolved();++budget) { actorTick(); }
    CHECK(App_Store_ConfigResolved());
    CHECK(App_Store_SetName("transintvl","2") == SVC_OK); actorTick();
    CHECK(STORE_instance.phase == STORE_SAVING);
    CHECK(App_Store_SetName("transintvl","3") == SVC_OK);
    for (budget=0U;budget<2000U && STORE_instance.saved==0U;++budget) { actorTick(); }
    CHECK(STORE_instance.saved==1U && STORE_instance.param.dirty && !App_Store_IsIdle());
    for (budget=0U;budget<2000U && !App_Store_IsIdle();++budget) { actorTick(); }
    CHECK(App_Store_IsIdle() && STORE_instance.saved==2U);
    reboot(); CHECK(loadValue()==3U);
    Store_ctor(); Store_start();
    for (budget=0U;budget<2000U && !App_Store_ConfigResolved();++budget) { actorTick(); }
    io_error=true; CHECK(App_Store_SetName("transintvl","4") == SVC_OK);
    for (budget=0U;budget<2000U && !STORE_instance.blocked;++budget) { actorTick(); }
    CHECK(STORE_instance.blocked && STORE_instance.param.dirty && !App_Store_IsIdle());
    CHECK(STORE_instance.failed==APP_CFG_STORE_RETRY+1U);
    io_error=false; CHECK(App_Store_SetName("transintvl","5") == SVC_OK);
    for (budget=0U;budget<2000U && !App_Store_IsIdle();++budget) { actorTick(); }
    CHECK(App_Store_IsIdle()); reboot(); CHECK(loadValue()==5U);
    puts("PASS actual AO edit-during-save, dirty retention, bounded retries and recovery");
}
int main(void) {
    uint8_t bytes[20], readback[20]; unsigned i, v; uint32_t p0;
    memset(media, 0xFF, sizeof(media)); reboot();
    CHECK(SVC_Storage_LayoutValid());
    CHECK(SVC_Storage_ProgramStart(SVC_STORAGE_APP, 0U, bytes, 1U) == SVC_ERR_PARAM);
    CHECK(SVC_Storage_EraseStart(SVC_STORAGE_APP, 0U, 4096U) == SVC_ERR_PARAM);
    CHECK(SVC_Storage_ReadStart(SVC_STORAGE_CONFIG, 0xFFFFU, bytes, 2U) == SVC_ERR_PARAM);
    CHECK(SVC_Storage_ReadStart(SVC_STORAGE_DATA, UINT32_MAX, bytes, 1U) == SVC_ERR_PARAM);
    CHECK(DRV_Storage_EraseStart(1U, 4096U) == DRV_ERR_PARAM);
    CHECK(DRV_Storage_ReadStart(0x3FFFFFUL, bytes, 2U) == DRV_ERR_PARAM);
    puts("PASS partition bounds, overflow, APP protection, erase alignment");
    memset(bytes, 0x5A, sizeof(bytes)); p0 = programs;
    CHECK(DRV_Storage_ProgramStart(0x10000U+250U, bytes, sizeof(bytes)) == DRV_OK); runDriver();
    CHECK(DRV_Storage_Result() == DRV_OK && programs - p0 == 2U);
    CHECK(DRV_Storage_ReadStart(0x10000U+250U, readback, sizeof(readback)) == DRV_OK); runDriver();
    CHECK(memcmp(bytes, readback, sizeof(bytes)) == 0);
    puts("PASS real driver cross-page program/read and WEL/BUSY sequencing");
    id_type = 0x70U; CHECK(DRV_Storage_ProbeStart() == DRV_OK); runDriver(); CHECK(DRV_Storage_Result() == DRV_ERR_NODEV);
    id_type = 0x40U; never_ready = true; now = UINT32_MAX - 200U;
    CHECK(DRV_Storage_ProbeStart() == DRV_OK); runDriver(); CHECK(DRV_Storage_Result() == DRV_ERR_TIMEOUT);
    never_ready = false; reboot(); CHECK(loadValue() == APP_CFG_MODEM_TRANS_INTERVAL_DEFAULT);
    startSave(1U); runJournal(); CHECK(SVC_ConfigJournal_Result(&SVC_configJournal) == SVC_OK);
    memcpy(baseline, media, sizeof(media));
    /* 252 B 正文加 4 B 提交标记，每个编程字节边界都切电重启。 */
    for (i = 0U; i <= 256U; ++i) {
        memcpy(media, baseline, sizeof(media)); reboot(); CHECK(loadValue() == 1U);
        cut_after = (int)i; startSave(2U); runJournal(); reboot(); v = loadValue();
        CHECK(v == (i >= 256U ? 2U : 1U));
    }
    puts("PASS 257 power-cut points across body/commit, old-or-new recovery");
    memcpy(media, baseline, sizeof(media)); reboot(); CHECK(loadValue() == 1U);
    startSave(2U); runJournal(); media[SVC_configJournal.latest + 16U] ^= 1U;
    reboot(); CHECK(loadValue() == 1U);
    startSave(3U); runJournal(); reboot(); CHECK(loadValue() == 3U);
    puts("PASS corrupted newest record fallback and partial-slot skip");
    /* 写满全部日志扇区，下一次保存触发回收。 */
    memset(media, 0xFF, APP_CFG_CONFIG_JOURNAL_SIZE); reboot(); (void)loadValue();
    for (i = 0U; i < CFG_SECTOR_COUNT * CFG_SLOTS_PER_SECTOR; ++i) {
        startSave(1U); runJournal(); CHECK(SVC_ConfigJournal_Result(&SVC_configJournal) == SVC_OK);
    }
    memcpy(baseline, media, sizeof(media));
    {
        unsigned const cuts[] = {0U,1U,255U,256U,4095U,4096U,4097U,4348U,4351U,4352U};
        for (i = 0U; i < sizeof(cuts)/sizeof(cuts[0]); ++i) {
            memcpy(media, baseline, sizeof(media)); reboot(); CHECK(loadValue() == 1U);
            cut_after = (int)cuts[i]; startSave(2U); runJournal(); reboot(); v = loadValue(); CHECK(v == 1U || v == 2U);
            CHECK(v == (cuts[i] >= 4352U ? 2U : 1U));
        }
    }
    puts("PASS sector rotation and 10 erase/reclaim power-cut points");
    memcpy(media, baseline, sizeof(media)); reboot(); CHECK(loadValue() == 1U);
    startSave(2U); io_error = true; runJournal(); CHECK(SVC_ConfigJournal_State(&SVC_configJournal) == SVC_STORAGE_FAILED);
    reboot(); CHECK(loadValue() == 1U);
    /* 传感器先占用，Flash 不得动缓冲/夺取所有权；指令完成后留出设备调度窗口。 */
    CHECK(DRV_Storage_ProbeStart() == DRV_OK); sensor_owned = true;
    for (i=0U;i<3U;++i) { now+=10U; DRV_Storage_Process(); CHECK(!owned); }
    sensor_owned = false; now+=10U; DRV_Storage_Process(); CHECK(owned);
    now+=10U; DRV_Storage_Process(); CHECK(!owned && DRV_Storage_State() == DRV_STORAGE_BUSY);
    sensor_owned = true; now+=10U; DRV_Storage_Process(); CHECK(!owned);
    sensor_owned = false; runDriver(); CHECK(DRV_Storage_Result() == DRV_OK);
    puts("PASS IO failure preserves old copy; shared-SPI BUSY and yield window");
    actorTests();
    printf("PASS ALL: 267 power-cut cases; program=%lu erase=%lu polls=%lu\n",
           (unsigned long)programs, (unsigned long)erases, (unsigned long)polls);
    return 0;
}
