/*============================================================================
* bsp_uart.h —— 板级支持层：USART1 收发与异步日志发送队列
*
* 层次定位：App → Service → Driver → **BSP** → HAL/CMSIS
*
* 职责：
*   1. 接收：1 字节中断接收 + 64 字节环形缓冲；中断只做「存数据 + 回调」
*   2. 发送（同步）：BSP_print*() 阻塞发送，仅用于初始化横幅与低频回显
*   3. 发送（异步）：BSP_logWrite() 入队，USART1 发送中断逐块搬出（日志通路）
*
* 缓冲区尺寸宏定义在**拥有者**（本头文件）里，数值取自 app_config.h；
* 调用方不得硬编码数字（见 07-接口约定#缓冲区与所有权）。
*===========================================================================*/
#ifndef BSP_UART_H
#define BSP_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "app_config.h"

/* 串口接收环形缓冲大小（中断写入，AO 读取）；数值来自 app_config.h */
#define BSP_RX_RING_SIZE     (APP_CFG_UART_RX_RING_SIZE)

/* 日志异步发送环形队列大小（字节）；数值来自 app_config.h */
#define BSP_LOG_TX_RING_SIZE (APP_CFG_LOG_TX_RING_SIZE)

/*--------------------------------------------------------------------------*/
/* USART 实例编号（BSP 自己的类型，不暴露 HAL）                              */
/*--------------------------------------------------------------------------*/
/** @brief BSP 串口实例编号。
*
* 为什么要有它：BLE 与 4G 各占哪一路 USART 属于**装配信息**，会随板子改版变化
* （2026-09-20 就对调过一次）。端口实现不再硬编码 huartN，改由装配点把编号传进
* BSP_*UartInit()；实例到 HAL 句柄的映射留在各端口 .c 里，避免 BSP 头文件暴露
* HAL 类型（见 docs_sensor/07-接口约定.md#BSP 层）。
*
* NOTE (取值)：与 app_config.h 的 APP_CFG_BLE_UART_ID / APP_CFG_MODEM_UART_ID
*   一一对应（1/2/3 = USART1/2/3）；bsp_uart.c 里有编译期检查。
*/
typedef enum {
    BSP_UART_NONE = 0,  /*!< 未分配（非法值）        */
    BSP_UART_1    = 1,  /*!< USART1：调试/AT 口      */
    BSP_UART_2    = 2,  /*!< USART2：PA2(TX)/PA3(RX) */
    BSP_UART_3    = 3   /*!< USART3：PB10(TX)/PB11(RX) */
} bsp_uart_id_t;

/*! USART1 收到 1 字节时的回调类型。由 App 装配点注册。
* @note 回调在 USART1 中断上下文执行，只允许做「存数据 + 投递事件」。
*/
typedef void (*BspUartRxSink)(uint8_t byte);

/* 接收 ---------------------------------------------------------------------*/

/*! 启动 1 字节的中断接收（由 BSP_init() 调用）。 */
void BSP_uartRxStart(void);

/*! 注册接收回调。
* @param sink 回调函数；传 NULL 表示注销
* @note 由 App 层装配点调用（App_start() 之前），BSP 自身不含应用逻辑。
*/
void BSP_uartRxSetSink(BspUartRxSink sink);

/*! 把收到的 1 字节压入接收环形缓冲。
* @note 仅限 USART1 接收中断（或其注册回调）调用。
*/
void BSP_uartRxPut(uint8_t byte);

/*! 若环形缓冲中有数据则取出一字节并返回 true（由 AO 在事件处理中调用）。 */
bool BSP_uartRxGet(uint8_t *byte);

/* 同步发送（阻塞，低频） ---------------------------------------------------*/

/*! 向调试串口发送以 '\0' 结尾的字符串（阻塞发送，适合初始化/低频调试输出）。 */
void BSP_print(char const *str);

/*! 向调试串口发送无符号十进制数（阻塞发送）。 */
void BSP_printU32(uint32_t value);

/*! 向调试串口发送字符串并追加 CRLF（阻塞发送）。 */
void BSP_printLine(char const *str);

/* 异步发送（日志队列） -----------------------------------------------------*/

/*! 把文本写入发送队列并立即返回（非阻塞，异步发送）。
* @note 仅限线程上下文（AO / 各层普通函数）调用；**禁止在中断里调用**。
*       队列满时丢弃该字节并累加丢弃计数（APP_CFG_LOG_DROP_WHEN_FULL）。
*/
void BSP_logWrite(char const *str);

/*! 阻塞等待发送队列排空（同时等待最后一次中断传输完成）。
* @warning 有界等待；只用于初始化横幅与命令行回显这类低频场景，
*          运行期日志请用 LOG_*（异步）。
*/
void BSP_logFlush(void);

/*! 因队列满被丢弃的字节数（诊断用）。 */
uint32_t BSP_logDropped(void);

/*! USART1（调试口）接收错误累计计数（ORE/FE/NE/PE）。
* @note 2026-09-20 新增：原先 USART1 出错后不再重装接收，会永久收不到输入且无迹可循
*       （见 bsp_uart.c 的 HAL_UART_ErrorCallback）；本计数用于把这种情况暴露出来。 */
uint32_t BSP_uartErrorCount(void);

#endif /* BSP_UART_H */
