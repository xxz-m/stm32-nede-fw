/*============================================================================
* bsp.c —— 板级支持层：QP 钩子、初始化与时间基准
*
* 层次定位：BSP 层。本文件只保留「框架要求的钩子」与板级初始化/时间基准；
*           USART1 收发与日志发送队列在 bsp_uart.c（见 bsp.h 的说明）。
*
* 职责：
*   1. QP/C 要求的 BSP 钩子：QF_onStartup、QF_onCleanup、QV_onIdle、错误钩子
*   2. BSP_init()：把 SysTick 配成 QP 节拍（100 Hz）、装载串口接收、打印横幅
*   3. 时间基准：BSP_uptimeMs() / BSP_delayMs()
*
* 注意：本文件不得包含 App/Service/Driver 的头文件（依赖单向向下）。
*       活动对象的装配在 App/app/app.c 的 App_start()。
*===========================================================================*/
#include "main.h"
#include "qpc.h"
#include "bsp.h"

/* QP/C 断言钩子。
* 注意：QP/C 6.x 要求应用提供 Q_onAssert()，并且必须在展开 QP 宏
* （Q_ASSERT 等）之前声明，故此处做前置声明（实现在本文件下方）。
*/
void Q_onAssert(char const * const module, int loc);

/*--------------------------------------------------------------------------*/
/* 时间基准                                                                  */
/*--------------------------------------------------------------------------*/
uint32_t BSP_uptimeMs(void) {
    return (uint32_t)HAL_GetTick(); /* 时基为 TIM1 的 1 ms 计数 */
}

void BSP_systemReset(void) {
    NVIC_SystemReset();
}

/*..........................................................................*/
void BSP_delayMs(uint32_t ms) {
    HAL_Delay(ms);
}

/*--------------------------------------------------------------------------*/
/* QP/C 要求的 BSP 钩子                                                      */
/*--------------------------------------------------------------------------*/

/*..........................................................................*/
void QF_onStartup(void) {
    /* 本工程没有启用 QS 跟踪，这里无需做事 */
}

/*..........................................................................*/
void QF_onCleanup(void) {
    /* 无需清理 */
}

/*..........................................................................*/
/**
* @brief QV 内核的空闲处理。
*
* QV 内核在关中断的情况下调用本回调，并要求回调返回时中断是打开的。
* QV_CPU_SLEEP() 正是这样做的：先置 PRIMASK，再执行 WFI，最后恢复中断使能，
* 因此唤醒用的那次中断不会丢。
*
* 注意：HAL 时基跑在 TIM1 上（1 ms 一次中断），所以即使应用完全空闲，CPU
* 也至少每 1 ms 被唤醒一次。若需要更深的睡眠，请延长 TIM1 周期或在进入低
* 功耗模式前关掉 HAL 时基。
*/
void QV_onIdle(void) {
    QV_CPU_SLEEP();
}

/*..........................................................................*/
/**
* @brief QP 断言失败钩子（致命错误）。
*/
Q_NORETURN Q_onError(char const * const module, int_t const id) {
    __disable_irq();
    BSP_printLine("");
    BSP_printLine("*** QP ASSERTION FAILED ***");
    BSP_print("module: ");
    BSP_printLine(module);
    BSP_print("id    : ");
    BSP_printU32((uint32_t)id);
    BSP_printLine("");
    for (;;) { /* 停在这里，便于调试器查看现场 */
    }
}

/*..........................................................................*/
/* 兼容 QP/C 6.x 已废弃的断言钩子 */
void Q_onAssert(char const * const module, int loc) {
    Q_onError(module, (int_t)loc);
}

/*--------------------------------------------------------------------------*/
/* 板级初始化                                                                */
/*--------------------------------------------------------------------------*/
/**
* @brief 初始化板级支持（与 QP 相关的部分）。
*/
/*..........................................................................*/
/**
* @brief 读取 RCC_CSR 的复位标志并转成短字符串，随后清标志（供下次启动判断）。
*
* NOTE (顺序)：看门狗/软件复位会同时置 PORRSTF，因此判断顺序按「越具体越优先」，
* 从 IWDG/WWDG/SOFT 开始，最后才落到 POR/PIN。
*/
static char const *bspResetCauseStr(void) {
    uint32_t const csr = RCC->CSR;
    char const *cause;

    if ((csr & RCC_CSR_IWDGRSTF) != 0U) {
        cause = "IWDG (independent watchdog)";
    }
    else if ((csr & RCC_CSR_WWDGRSTF) != 0U) {
        cause = "WWDG (window watchdog)";
    }
    else if ((csr & RCC_CSR_SFTRSTF) != 0U) {
        cause = "SOFT (software reset)";
    }
    else if ((csr & RCC_CSR_PORRSTF) != 0U) {
        cause = "POR (power-on reset)";
    }
    else if ((csr & RCC_CSR_PINRSTF) != 0U) {
        cause = "PIN (nRST pin)";
    }
    else if ((csr & RCC_CSR_LPWRRSTF) != 0U) {
        cause = "LPWR (low-power)";
    }
    else {
        cause = "unknown";
    }

    RCC->CSR |= RCC_CSR_RMVF;   /* 清标志，让下次启动的原因准确 */

    return cause;
}

void BSP_init(void) {
    BSP_printLine("");
    /* 启动横幅（ASCII only，见 AGENTS 第 5.2 节）：版本 / 目标 / 内核 / 复位原因 / 通道。
    * NOTE (设计)：AO 列表与 "boot ok" 由 App 层打印（BSP 不认识 App，依赖单向向下）。 */
    BSP_printLine("==== sensor-node-fw ====");
    BSP_printLine("ver    : " APP_CFG_FW_VERSION "  (built " APP_CFG_FW_BUILD_DATE ")");
    BSP_printLine("target : STM32F103RCT6  72 MHz  flash 256K/ram 48K");
    BSP_printLine("kernel : QP/C 6.9.1  QV (cooperative, single stack)");
    BSP_print("reset  : ");
    BSP_print(bspResetCauseStr());
    BSP_printLine("");
    BSP_printLine("chan   : USART1 115200 8N1  (debug/AT only)");

    /* I2C1 主机：使能中断（I2C 中断的 IRQ 入口在 bsp_i2c.c，见其文件头 NOTE）
    * 并清理上次复位可能留下的 BUSY。参数存储的读写在它之上跑。 */
    (void)BSP_i2cInit();

    /* 共享 SPI1：DMA 事务完成由 AO 周期检查，不增加任何 QF-aware IRQ。
     * 两个设备槽都在 BSP_spiInit() 里登记：W25Q32（CS=PA8）与 SCL3300（CS=PA4）。 */
    if (BSP_spiInit() != BSP_OK) {
        BSP_printLine("spi    : SPI1 initialization failed");
    } else {
        BSP_printLine("spi    : SPI1 PA5/PA6/PA7, flash CS PA8, inclinometer CS PA4");
    }

    BSP_printLine("------------------------");

    /* HAL 时基已经挪到 TIM1，因此 HAL_Init() 不会启动 SysTick，这里把 SysTick
    * 配成 QP 的时钟节拍。HAL_SYSTICK_Config() 同时会把 SysTick 优先级设为
    * TICK_INT_PRIORITY（15），属于 QF-aware（>= QF_AWARE_ISR_CMSIS_PRI = 3）。
    */
    if (HAL_SYSTICK_Config(SystemCoreClock / BSP_TICKS_PER_SEC) != 0U) {
        BSP_printLine("*** BSP: SysTick config failed ***");
        for (;;) {
        }
    }

    BSP_uartRxStart(); /* 装载串口中断接收 */
}
