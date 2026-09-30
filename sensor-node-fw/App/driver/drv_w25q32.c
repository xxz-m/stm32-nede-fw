/*============================================================================
 * drv_w25q32.c —— Driver 层：W25Q32JV 非阻塞读/编程/擦除
 * 目标器件：W25Q32JVSNIQ，4 MiB/256 B 页/4 KiB 扇区；内核无关，无 HAL/QP 依赖。
 * 来源：Winbond W25Q32JV Rev J，标准 SPI 03/02/20/05/06/9F 指令。
 * NOTE (共享)：每条指令后释放 SPI，内部擦写期间仅周期性查询 BUSY，不占传感器总线。
 *===========================================================================*/
#include "drv_w25q32.h"
#include "bsp.h"
#include <string.h>

typedef enum { P_READY, P_ID, P_READ, P_WREN, P_WEL, P_WRITE, P_WAIT } Phase;
static struct {
    DrvStorageState state;
    drv_status_t result;
    W25Operation op;
    Phase phase;
    bool pending;
    uint32_t address, length, position, started, timeout, job_started;
    uint16_t chunk;
    uint8_t *read;
    uint8_t const *write;
} l_job;

/** @brief 结束作业；事务已经取走，芯片若仍 BUSY，下次 Start 会先检查。 */
static void finish(drv_status_t result) {
    l_job.result = result;
    l_job.state = result == DRV_OK ? DRV_STORAGE_DONE : DRV_STORAGE_FAILED;
}

/** @brief 准备本段页边界；读也分段，限制一次占用 SPI 的时间。 */
static void prepare(void) {
    uint32_t left = l_job.length - l_job.position;
    uint32_t room = APP_CFG_FLASH_PAGE_SIZE - ((l_job.address + l_job.position) % APP_CFG_FLASH_PAGE_SIZE);
    l_job.chunk = (uint16_t)(left < room ? left : room);
    l_job.phase = l_job.op == W25_PROBE ? P_ID : (l_job.op == W25_READ ? P_READ : P_WREN);
}

/** @brief 初始化状态；不对芯片发复位/解锁，避免打断掉电前尚未完成的擦除。 */
void DRV_W25Q32_Init(void) { memset(&l_job, 0, sizeof(l_job)); }

/** @brief 校验地址、长度和擦除对齐，建立作业但不阻塞。 */
drv_status_t DRV_W25Q32_Start(W25Operation op, uint32_t addr, uint8_t *read,
                             uint8_t const *write, uint32_t length) {
    if (l_job.state == DRV_STORAGE_BUSY) { return DRV_ERR_BUSY; }
    if (op < W25_PROBE || op > W25_ERASE || (op != W25_PROBE &&
        (length == 0U || addr >= APP_CFG_FLASH_CAPACITY || length > APP_CFG_FLASH_CAPACITY - addr)) ||
        (op == W25_READ && read == (uint8_t *)0) ||
        (op == W25_PROGRAM && write == (uint8_t const *)0) ||
        (op == W25_ERASE && (addr % APP_CFG_FLASH_SECTOR_SIZE != 0U || length % APP_CFG_FLASH_SECTOR_SIZE != 0U))) {
        return DRV_ERR_PARAM;
    }
    l_job.state = DRV_STORAGE_BUSY;
    l_job.result = DRV_OK;
    l_job.op = op; l_job.address = addr; l_job.length = length; l_job.position = 0U;
    l_job.read = read; l_job.write = write; l_job.pending = false; l_job.phase = P_READY;
    l_job.started = l_job.job_started = BSP_uptimeMs();
    l_job.timeout = APP_CFG_FLASH_ERASE_TIMEOUT_MS;
    return DRV_OK;
}

/** @brief 发起当前相位的一条指令；总线被传感器占用时保留相位，下拍再试。 */
static drv_status_t issue(void) {
    uint32_t addr = l_job.address + l_job.position;
    switch (l_job.phase) {
        case P_READY: case P_WEL: case P_WAIT: return DRV_W25Q32_IoStart(0x05U, 0U, false, 0, 1U);
        case P_ID: return DRV_W25Q32_IoStart(0x9FU, 0U, false, 0, 3U);
        case P_READ: return DRV_W25Q32_IoStart(0x03U, addr, true, 0, l_job.chunk);
        case P_WREN: return DRV_W25Q32_IoStart(0x06U, 0U, false, 0, 0U);
        case P_WRITE:
            return DRV_W25Q32_IoStart(l_job.op == W25_ERASE ? 0x20U : 0x02U, addr, true,
                                      l_job.op == W25_ERASE ? 0 : l_job.write + l_job.position,
                                      l_job.op == W25_ERASE ? 0U : l_job.chunk);
        default: return DRV_ERR_PARAM;
    }
}

/** @brief 处理已完成指令；任何相位都不自旋等待 BUSY/WEL。 */
static void completed(void) {
    uint8_t const *data = DRV_W25Q32_IoData();
    switch (l_job.phase) {
        case P_READY:
            if ((data[0] & 1U) == 0U) { prepare(); }
            break;
        case P_ID:
            /* SNIQ 普通 JV 为 EF4016；DTR/IM 的 EF7016 不作为本款芯片接受。 */
            finish(data[0] == 0xEFU && data[1] == 0x40U && data[2] == 0x16U ? DRV_OK : DRV_ERR_NODEV);
            break;
        case P_WREN: l_job.phase = P_WEL; break;
        case P_WEL:
            if ((data[0] & 3U) != 2U) { finish(DRV_ERR_BUS); }
            else { l_job.phase = P_WRITE; }
            break;
        case P_WRITE:
            l_job.started = BSP_uptimeMs();
            l_job.timeout = l_job.op == W25_ERASE ? APP_CFG_FLASH_ERASE_TIMEOUT_MS : APP_CFG_FLASH_PROGRAM_TIMEOUT_MS;
            l_job.phase = P_WAIT;
            break;
        case P_READ:
            memcpy(l_job.read + l_job.position, data, l_job.chunk);
            l_job.position += l_job.chunk;
            if (l_job.position == l_job.length) { finish(DRV_OK); } else { prepare(); }
            break;
        case P_WAIT:
            if ((data[0] & 1U) == 0U) {
                l_job.position += l_job.op == W25_ERASE ? APP_CFG_FLASH_SECTOR_SIZE : l_job.chunk;
                if (l_job.position == l_job.length) { finish(DRV_OK); } else { prepare(); }
            }
            break;
        default: finish(DRV_ERR_PARAM); break;
    }
}

/** @brief 推进一条指令；所有超时采用回绕安全的差值，挂起事务先取结果再结束。 */
void DRV_W25Q32_Process(void) {
    drv_status_t st;
    if (l_job.state != DRV_STORAGE_BUSY) { return; }
    if (l_job.pending) {
        if (!DRV_W25Q32_IoResult(&st)) { return; }
        l_job.pending = false;
        if (st != DRV_OK) { finish(st); return; }
        completed();
        /* 一条指令完成后主动让出一拍；不能立即重发查询把传感器长期饿死。 */
        return;
    }
    if ((l_job.phase == P_READY || l_job.phase == P_WAIT) &&
        (uint32_t)(BSP_uptimeMs() - l_job.started) >= l_job.timeout) {
        finish(DRV_ERR_TIMEOUT); return;
    }
    /* 防止共享总线一直被另一设备占用；大作业的整体期限由调用方分段预算。 */
    if ((uint32_t)(BSP_uptimeMs() - l_job.job_started) >= APP_CFG_FLASH_JOB_TIMEOUT_MS) {
        finish(DRV_ERR_TIMEOUT); return;
    }
    st = issue();
    if (st == DRV_OK) { l_job.pending = true; }
    else if (st != DRV_ERR_BUSY) { finish(st); }
}
/** @brief 返回当前作业状态。 */
DrvStorageState DRV_W25Q32_State(void) { return l_job.state; }
/** @brief 返回最近作业结果。 */
drv_status_t DRV_W25Q32_Result(void) { return l_job.result; }
