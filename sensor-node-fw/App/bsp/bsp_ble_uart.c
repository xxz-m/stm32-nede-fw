/*============================================================================
* bsp_ble_uart.c —— BLE 模组串口非阻塞端口（实例由装配点注入）
* 层次定位：BSP；PB10/PB11 或 PA2/PA3 由 APP_CFG_BLE_UART_ID 决定，
*           115200 8N1，单字节中断接收 + 分块中断发送。
*
* 设计要点：
*   - 实例不写死：l_uart 由 BSP_bleUartInit() 按编号解析，改接线只动配置
*   - 接收：单字节中断接收，收满即重新装载；出错后顺手恢复链路
*   - 发送：非阻塞入队，空闲时由 HAL_UART_Transmit_IT 分块搬出
*   - VG6_EN(PB1)：模组电源使能，初始化时配成推挽输出并拉高
*===========================================================================*/
#include "bsp_ble_uart.h"
#include "app_config.h"
#include "stm32f1xx_hal.h"
#include "usart.h"
#include <stdbool.h>

/*! 本端口占用的串口实例（未初始化时为 NULL，所有入口先判空） */
static UART_HandleTypeDef *l_uart;
static bsp_uart_id_t l_uartId = BSP_UART_NONE;

static uint8_t l_rxByte;
static uint8_t l_ring[APP_CFG_BLE_RX_RING_SIZE];
static volatile uint16_t l_head;
static volatile uint16_t l_tail;
static volatile uint32_t l_overflow;
static volatile uint32_t l_errorCount;
static volatile uint32_t l_rxCount;
static volatile uint32_t l_txCount;
static BspBleRxSink l_sink;
static uint8_t l_txRing[APP_CFG_BLE_TX_RING_SIZE];
static volatile uint16_t l_txHead;
static volatile uint16_t l_txTail;
static volatile uint16_t l_txChunk;
static volatile bool l_txBusy;

/*--------------------------------------------------------------------------*/
/* 实例解析与模组电源                                                        */
/*--------------------------------------------------------------------------*/
/** @brief 把 BSP 实例编号翻成 HAL 句柄；非法编号返回 NULL。
* @note  映射放在 .c 里，BSP 头文件因此不需要出现 HAL 类型。 */
static UART_HandleTypeDef *uartHandle(bsp_uart_id_t uart) {
    if (uart == BSP_UART_2) {
        return &huart2;
    }
    if (uart == BSP_UART_3) {
        return &huart3;
    }
    return (UART_HandleTypeDef *)0;   /* USART1 是调试口，不分配给业务端口 */
}

/** @brief VG6_EN(PB1) 拉高：模组电源使能，高开启、低关闭。
*
* NOTE (为什么写在 BSP 里)：这是「上电时序」而不是业务逻辑，必须发生在装载
*   接收之前；同时 .ioc 里已把 PB1 登记成 GPIO_Output（初始 High），CubeMX
*   再生成后 gpio.c 会配置同一引脚——本函数是幂等的，重复配置无害。
* NOTE (电平)：低电平会把 VG6328 断电，现象是收不到任何字节且 uart_err = 0，
*   属于最难判读的静默故障，所以这里显式拉高而不是依赖复位默认值。 */
static void blePowerEnable(void) {
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin   = GPIO_PIN_1;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_1, GPIO_PIN_SET);
}

/*--------------------------------------------------------------------------*/
/* 发送                                                                      */
/*--------------------------------------------------------------------------*/
static void txKick(void) {
    uint16_t length;

    if ((l_txBusy) || (l_txHead == l_txTail) || (l_uart == (UART_HandleTypeDef *)0)) {
        return;
    }
    length = (l_txHead > l_txTail) ? (uint16_t)(l_txHead - l_txTail)
                                   : (uint16_t)(APP_CFG_BLE_TX_RING_SIZE - l_txTail);
    l_txChunk = length;
    l_txBusy = true;
    if (HAL_UART_Transmit_IT(l_uart, &l_txRing[l_txTail], length) != HAL_OK) {
        l_txBusy = false;
    }
}

static ble_port_status_t portWrite(void *context, uint8_t const *data, size_t len) {
    uint16_t head;
    uint16_t free;
    size_t i;
    (void)context;
    if ((data == (uint8_t const *)0) || (len == 0U)) return BLE_PORT_ERROR;
    __disable_irq();
    head = l_txHead;
    free = (l_txTail > head) ? (uint16_t)(l_txTail - head - 1U)
                             : (uint16_t)(APP_CFG_BLE_TX_RING_SIZE - head + l_txTail - 1U);
    if (len > free) { __enable_irq(); return BLE_PORT_BUSY; }
    for (i = 0U; i < len; ++i) {
        l_txRing[head] = data[i];
        head = (uint16_t)((head + 1U) % APP_CFG_BLE_TX_RING_SIZE);
    }
    l_txHead = head;
    __enable_irq();
    txKick();
    return BLE_PORT_OK;
}

/*--------------------------------------------------------------------------*/
/* 接收                                                                      */
/*--------------------------------------------------------------------------*/
static size_t portRead(void *context, uint8_t *out, size_t cap) {
    size_t count = 0U;
    (void)context;
    if (l_uart == (UART_HandleTypeDef *)0) return 0U;
    /* 上电或出错后接收链路可能没在跑，这里顺手拉起来（与 USART1 的做法一致）。 */
    if (l_uart->RxState != HAL_UART_STATE_BUSY_RX) {
        __HAL_UART_CLEAR_OREFLAG(l_uart);
        __HAL_UART_FLUSH_DRREGISTER(l_uart);
        (void)HAL_UART_Receive_IT(l_uart, &l_rxByte, 1U);
    }
    while ((count < cap) && (l_tail != l_head)) {
        out[count++] = l_ring[l_tail];
        l_tail = (uint16_t)((l_tail + 1U) % APP_CFG_BLE_RX_RING_SIZE);
    }
    return count;
}

static uint32_t portTick(void *context) { (void)context; return HAL_GetTick(); }
static void portTrace(void *context, char const *text) { (void)context; (void)text; }

/*--------------------------------------------------------------------------*/
/* 初始化与分派                                                              */
/*--------------------------------------------------------------------------*/
void BSP_bleUartInit(BlePort *port, bsp_uart_id_t uart) {
    if (port == (BlePort *)0) {
        return;
    }
    l_uart = uartHandle(uart);
    if (l_uart == (UART_HandleTypeDef *)0) {
        return;   /* 编号非法：不配置任何外设，也不填 Port（调用方按未就绪处理） */
    }
    l_uartId = uart;
    l_head = 0U; l_tail = 0U; l_overflow = 0U;
    l_errorCount = 0U; l_rxCount = 0U; l_txCount = 0U;
    l_txHead = 0U; l_txTail = 0U; l_txChunk = 0U; l_txBusy = false;
    blePowerEnable();   /* 先给模组上电，再配置线速率与接收 */

    /* NOTE (波特率必须在这里真正写进外设，2026-09-27)：CubeMX 按 .ioc 生成 huart2 的
    * 速率，而 `.ioc` 里 USART2 恰好就是 115200、与 APP_CFG_BLE_UART_BAUD 相同，所以
    * 原先「不应用这个宏」在这块板上看不出问题。但 4G 端口正是因为同样没应用宏而跑在
    * 115200（应为 9600）、模组完全不应答——两个端口必须用同一套规矩，否则换实例或
    * 换模组时会再次踩同一个坑。理由详见 bsp_modem_uart.c 的对应 NOTE。 */
    l_uart->Init.BaudRate = (uint32_t)APP_CFG_BLE_UART_BAUD;
    if (HAL_UART_Init(l_uart) != HAL_OK) {
        ++l_errorCount;
    }

    (void)HAL_UART_Receive_IT(l_uart, &l_rxByte, 1U);
    port->context = l_uart;
    port->write = &portWrite;
    port->read = &portRead;
    port->get_tick_ms = &portTick;
    port->trace = &portTrace;
}

bool BSP_bleUartIsInstance(bsp_uart_id_t uart) {
    return (uart != BSP_UART_NONE) && (uart == l_uartId);
}

void BSP_bleUartSetSink(BspBleRxSink sink) { l_sink = sink; }

void BSP_bleUartRxComplete(void) {
    uint16_t next = (uint16_t)((l_head + 1U) % APP_CFG_BLE_RX_RING_SIZE);
    if (next == l_tail) ++l_overflow;
    else { l_ring[l_head] = l_rxByte; l_head = next; }
    ++l_rxCount;
    (void)HAL_UART_Receive_IT(l_uart, &l_rxByte, 1U);
    if (l_sink != (BspBleRxSink)0) l_sink();
}

void BSP_bleUartTxComplete(void) {
    l_txCount += l_txChunk;
    l_txTail = (uint16_t)((l_txTail + l_txChunk) % APP_CFG_BLE_TX_RING_SIZE);
    l_txBusy = false;
    txKick();
}

void BSP_bleUartError(void) {
    ++l_errorCount;
    __HAL_UART_CLEAR_OREFLAG(l_uart);
    __HAL_UART_FLUSH_DRREGISTER(l_uart);
    (void)HAL_UART_Receive_IT(l_uart, &l_rxByte, 1U);
}

uint32_t BSP_bleUartOverflow(void) { return l_overflow; }
uint32_t BSP_bleUartErrorCount(void) { return l_errorCount; }
uint32_t BSP_bleUartRxCount(void) { return l_rxCount; }
uint32_t BSP_bleUartTxCount(void) { return l_txCount; }
uint32_t BSP_bleUartBaud(void) {
    return (l_uart != (UART_HandleTypeDef *)0) ? l_uart->Init.BaudRate : 0U;
}
