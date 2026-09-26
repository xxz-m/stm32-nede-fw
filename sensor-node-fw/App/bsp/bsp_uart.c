/*============================================================================
* bsp_uart.c —— 板级支持层：USART1 收发实现（含异步日志发送队列）
*
* 层次定位：BSP 层。唯一接触 USART1 与 HAL 的地方。
*
* 数据通路：
*   接收：USART1 中断 -> HAL_UART_RxCpltCallback -> 注册的回调（App）-> 环形缓冲
*   发送（同步）：BSP_print*() -> HAL_UART_Transmit（带超时）
*   发送（异步）：BSP_logWrite() -> 发送环形队列 -> HAL_UART_Transmit_IT
*                -> HAL_UART_TxCpltCallback 继续搬下一段
*
* 注意：本文件不得包含 App/Service/Driver 的头文件（依赖单向向下）。
*===========================================================================*/
#include "main.h"
#include "usart.h"
#include "bsp.h"

#include <string.h>

/*--------------------------------------------------------------------------*/
/* 内部状态                                                                  */
/*--------------------------------------------------------------------------*/

/* 接收方向 */
static uint8_t rxByte;                    /* HAL 中断接收的 1 字节落点      */
static volatile uint8_t  rxRing[BSP_RX_RING_SIZE];
static volatile uint16_t rxHead;          /* 由接收中断写入                 */
static volatile uint16_t rxTail;          /* 由 AO 读取                     */
static volatile uint32_t rxErrCount;      /* USART1 接收错误累计（诊断用）   */
static BspUartRxSink s_rxSink;            /* App 层注册的接收回调           */

/* 发送方向：日志异步队列（环形 FIFO） */
static volatile uint8_t  txRing[BSP_LOG_TX_RING_SIZE];
static volatile uint16_t txHead;    /* 生产者：各层调用 BSP_logWrite()  */
static volatile uint16_t txTail;    /* 消费者：USART1 发送中断          */
static volatile uint16_t txChunk;   /* 本次 IT 传输的字节数             */
static volatile bool     txBusy;    /* 是否已有 IT 传输在进行            */
static volatile uint32_t txDropped; /* 队列满被丢弃的字节数（诊断用）     */

/*--------------------------------------------------------------------------*/
/* 实例编号 <-> HAL 句柄                                                     */
/*--------------------------------------------------------------------------*/
/** @brief 把 HAL 的实例指针翻成 BSP 的实例编号；未知实例返回 BSP_UART_NONE。
*
* 为什么要这层：BLE 与 4G 各占哪一路 USART 是装配信息（2026-09-20 对调过），
* 端口实现不再硬编码 huartN。映射放在 .c 里，BSP 头文件因此不出现 HAL 类型
* （见 docs_sensor/07-接口约定.md#BSP 层）。
*/
static bsp_uart_id_t uartIdOf(USART_TypeDef *instance) {
    if (instance == USART1) { return BSP_UART_1; }
    if (instance == USART2) { return BSP_UART_2; }
    if (instance == USART3) { return BSP_UART_3; }
    return BSP_UART_NONE;
}

/* 编译期检查：BSP 的实例编号必须与 app_config.h 里的配置值一致——装配点用
* (bsp_uart_id_t)APP_CFG_BLE_UART_ID 之类的常量传入，对不上就会静默绑错串口。 */
typedef char bsp_uartId_assert[(BSP_UART_2 == 2) && (BSP_UART_3 == 3) ? 1 : -1];

/* 编译期检查：两个业务端口不得占用同一个串口实例。
* NOTE (为什么加，2026-09-27)：每个端口会在自己的 `BSP_*UartInit()` 里把**自己的**
*   波特率写进所占实例（BLE 115200、4G 按后端分档），两者共用一路时后初始化的那个会
*   静默覆盖前一个，现象是其中一个模组永远不应答、而日志看不出异常。 */
#if (APP_CFG_BLE_ENABLE != 0) && (APP_CFG_MODEM_ENABLE != 0) && \
    (APP_CFG_BLE_UART_ID == APP_CFG_MODEM_UART_ID)
#error "APP_CFG_BLE_UART_ID 与 APP_CFG_MODEM_UART_ID 不能相同：两个端口会互相覆盖线速率"
#endif

/*--------------------------------------------------------------------------*/
/* 同步发送（阻塞）                                                          */
/*--------------------------------------------------------------------------*/
void BSP_print(char const *str) {
    /* 说明：带超时的阻塞发送只服务初始化与低频调试输出，单次最长阻塞约一个字
    * 节时间（115200 下约 87 µs）。运行期请用 LOG_* 异步日志。
    *
    * 为避免与异步队列的数据交错，这里先把异步队列排空再阻塞发送。
    */
    BSP_logFlush();

    (void)HAL_UART_Transmit(&huart1, (uint8_t *)str, (uint16_t)strlen(str), 100U);
}

/*..........................................................................*/
void BSP_printU32(uint32_t value) {
    char buf[11]; /* 最多 10 位数字 + '\0' */
    char *p = &buf[sizeof(buf) - 1U];

    *p = '\0';
    do {
        *(--p) = (char)('0' + (char)(value % 10U));
        value /= 10U;
    } while (value != 0U);

    BSP_print(p);
}

/*..........................................................................*/
void BSP_printLine(char const *str) {
    BSP_print(str);
    BSP_print("\r\n");
}

/*--------------------------------------------------------------------------*/
/* 接收                                                                      */
/*--------------------------------------------------------------------------*/
void BSP_uartRxStart(void) {
    rxHead = 0U;
    rxTail = 0U;
    rxErrCount = 0U;
    (void)HAL_UART_Receive_IT(&huart1, &rxByte, 1U); /* 装载 1 字节中断接收 */
}

/*..........................................................................*/
void BSP_uartRxSetSink(BspUartRxSink sink) {
    s_rxSink = sink;
}

/*..........................................................................*/
void BSP_uartRxPut(uint8_t byte) {
    uint16_t next = (uint16_t)((rxHead + 1U) % BSP_RX_RING_SIZE);

    /* 单生产者（中断）单消费者（AO）无锁环形缓冲：中断只动 rxHead，
    * AO 只动 rxTail，因此这里不需要临界区。
    */
    if (next != rxTail) {   /* 环形缓冲未满？ */
        rxRing[rxHead] = byte;
        rxHead = next;
    }
    /* 否则静默丢弃该字节（缓冲满，消费者来不及处理），中断里不阻塞 */
}

/*..........................................................................*/
bool BSP_uartRxGet(uint8_t *byte) {
    if ((byte == (uint8_t *)0) || (rxHead == rxTail)) { /* 缓冲为空？ */
        return false;
    }
    *byte = rxRing[rxTail];
    rxTail = (uint16_t)((rxTail + 1U) % BSP_RX_RING_SIZE);
    return true;
}

/*..........................................................................*/
/**
* @brief USART1 收到 1 字节后由 HAL 在中断上下文调用的回调。
*
* BSP 只负责「把字节交给应用注册的回调」，再由回调去存数据与投递事件，
* 这样 BSP 不需要认识任何活动对象（依赖单向向下）。
*
* @note USART1 中断必须是 "QF-aware" 的，即抢占优先级数值大于等于
* QF_AWARE_ISR_CMSIS_PRI（STM32F1 有 4 位优先级时该值为 3）。当前配置为 5。
*/
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart) {
    bsp_uart_id_t id;

    if (huart->Instance == USART1) {
        if (s_rxSink != (BspUartRxSink)0) {
            s_rxSink(rxByte);
        }
        (void)HAL_UART_Receive_IT(&huart1, &rxByte, 1U); /* 重新装载接收 */
        return;
    }

    /* NOTE (分派方式)：按**实例编号**分派，而不是写死 huartN——BLE 与 4G 各占
     * 哪一路由装配点决定（app_config.h 的 APP_CFG_*_UART_ID）。本文件不认识业务，
     * 只把「这一路收到了一字节」转给占用该实例的端口。 */
    id = uartIdOf(huart->Instance);
    if (BSP_bleUartIsInstance(id)) {
        BSP_bleUartRxComplete();
    }
#if (APP_CFG_MODEM_ENABLE != 0U)
    /* 关掉 4G 时本分支连同 bsp_modem_uart.c 一起在编译期消失（见 app_config.h）。 */
    else if (BSP_modemUartIsInstance(id)) {
        BSP_modemUartRxComplete();
    }
#endif
}

/*--------------------------------------------------------------------------*/
/* 发送（异步日志队列 + 发送中断）                                            */
/*--------------------------------------------------------------------------*/

/*..........................................................................*/
/**
* @brief 内部：把队列中已有的数据启动一次中断发送。
*
* 本次只发「从 txTail 到环尾」这一段，避免环形回绕导致 HAL 需要两次传输；
* 剩余部分由 HAL_UART_TxCpltCallback() 继续启动。
* @note 可在中断上下文调用（TxCplt 里就是这么用的）。
*/
static void txKick(void) {
    uint16_t tail;
    uint16_t head;
    uint16_t len;

    if (txBusy) {
        return; /* 已有传输在进行，完成回调会继续 */
    }

    tail = txTail;
    head = txHead;
    if (tail == head) {
        return; /* 队列空 */
    }

    len = (head > tail) ? (uint16_t)(head - tail)
                        : (uint16_t)(BSP_LOG_TX_RING_SIZE - tail);

    txChunk = len;
    txBusy = true;
    if (HAL_UART_Transmit_IT(&huart1, (uint8_t *)&txRing[tail], len) != HAL_OK) {
        txBusy = false; /* 启动失败：数据留在队列里，下次再试 */
    }
}

/*..........................................................................*/
void BSP_logWrite(char const *str) {
    uint16_t head;
    uint16_t next;

    if (str == (char const *)0) {
        return;
    }

    /* 多生产者（线程上下文）+ 单消费者（发送中断）：入队侧用短临界区保护索引。
    * 每字节一次临界区，范围只有几条语句（见 07-接口约定#中断安全与临界区）。
    */
    for ( ; *str != '\0'; ++str) {
        __disable_irq();
        head = txHead;
        next = (uint16_t)((head + 1U) % BSP_LOG_TX_RING_SIZE);
        if (next == txTail) {
            ++txDropped;            /* 队列满：丢弃该字节（不阻塞调用者） */
        }
        else {
            txRing[head] = (uint8_t)*str;
            txHead = next;
        }
        __enable_irq();
    }

    txKick();
}

/*..........................................................................*/
uint32_t BSP_logDropped(void) {
    return txDropped;
}

/*..........................................................................*/
uint32_t BSP_uartErrorCount(void) {
    return rxErrCount;
}

/*..........................................................................*/
void BSP_logFlush(void) {
    uint32_t guard = 0U;

    txKick();
    /* 有界等待：中断在等待期间照常运行，把队列搬空 */
    while (((txHead != txTail) || txBusy) && (guard < 500000U)) {
        ++guard;
    }
}

/*..........................................................................*/
/**
* @brief 发送完成回调（USART1 中断上下文）：推进队列尾指针并继续发送。
*/
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
    bsp_uart_id_t id;

    if (huart->Instance == USART1) {
        txTail = (uint16_t)((txTail + txChunk) % BSP_LOG_TX_RING_SIZE);
        txBusy = false;
        txKick();   /* 队列里还有数据就接着发（异步，不阻塞） */
        return;
    }

    id = uartIdOf(huart->Instance);
    if (BSP_bleUartIsInstance(id)) {
        BSP_bleUartTxComplete();
    }
#if (APP_CFG_MODEM_ENABLE != 0U)
    else if (BSP_modemUartIsInstance(id)) { BSP_modemUartTxComplete(); }
#endif
}

/**
* @brief UART 错误回调：恢复各路的单字节接收。
*
* NOTE (2026-09-20 修)：原先只处理 USART3/USART2，**漏了 USART1**。而 HAL 在
*   Receive_IT 期间收到 ORE/FE/NE/PE 时会结束本次接收并关掉 RXNE 中断，若无人
*   重装，USART1 的接收链路就永久停在 READY——现象是「调试口还能打印、却再也收不到
*   任何输入」，且 uart_err 之类的计数一个都不动，属于最难判读的一类静默故障。
*   这里补上恢复并累计 BSP_uartErrorCount()。
*/
void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    bsp_uart_id_t id;

    if (huart->Instance == USART1) {
        ++rxErrCount;
        __HAL_UART_CLEAR_OREFLAG(&huart1);
        __HAL_UART_FLUSH_DRREGISTER(&huart1);
        (void)HAL_UART_Receive_IT(&huart1, &rxByte, 1U);   /* 错误后恢复接收 */
        return;
    }

    id = uartIdOf(huart->Instance);
    if (BSP_bleUartIsInstance(id)) {
        BSP_bleUartError();
    }
#if (APP_CFG_MODEM_ENABLE != 0U)
    else if (BSP_modemUartIsInstance(id)) { BSP_modemUartError(); }
#endif
}
