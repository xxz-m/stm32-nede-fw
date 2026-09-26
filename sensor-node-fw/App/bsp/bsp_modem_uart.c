/*============================================================================
* bsp_modem_uart.c —— 4G 模组非阻塞端口（实例由装配点注入）
* 层次定位：BSP；实例由 APP_CFG_MODEM_UART_ID 决定（当前 USART3 = PB10/PB11），
*           波特率由 APP_CFG_MODEM_UART_BAUD 按后端分档，8N1，
*           单字节中断接收 + 分块中断发送。
* 目标硬件：STM32F103RCT6
* 所属内核：QV（协作式）
*
* 设计要点：
*   - 实例不写死：l_uart 由 BSP_modemUartInit() 按编号解析，改接线只动配置
*   - 接收：单字节中断接收，收满即重新装载；sink 只投事件，绝不解析
*   - 发送：非阻塞入队，空闲时由 HAL_UART_Transmit_IT 分块搬出（每块 <= 32 B，
*           避免一次中断里搬大量数据把 100 Hz 节拍挤歪）
*   - Port 契约：write() 要么整包接受、要么返回 MODEM_PORT_BUSY，绝不只收一半
*   - 溢出与错误只累计计数，诊断读数由上层 AO 通过 BSP_modem*Count() 取
*   - HAL 回调统一在 bsp_uart.c 里定义，本文件只提供按 Instance 分发的入口函数
*===========================================================================*/
#include "bsp_modem_uart.h"

#if (APP_CFG_MODEM_ENABLE != 0U)

#include "stm32f1xx_hal.h"
#include "usart.h"
#include <stdbool.h>

/*! 环形缓冲尺寸必须是 2 的幂，才能用掩码替代取模 */
#if ((APP_CFG_MODEM_RX_RING_SIZE & (APP_CFG_MODEM_RX_RING_SIZE - 1U)) != 0U)
#error "APP_CFG_MODEM_RX_RING_SIZE must be a power of two"
#endif
#if ((APP_CFG_MODEM_TX_RING_SIZE & (APP_CFG_MODEM_TX_RING_SIZE - 1U)) != 0U)
#error "APP_CFG_MODEM_TX_RING_SIZE must be a power of two"
#endif

/*! 每次发送中断搬运的最大字节数（限制中断时长） */
#define MODEM_TX_CHUNK_MAX  (32U)

/*! 本端口占用的串口实例（由 BSP_modemUartInit() 按编号解析） */
static UART_HandleTypeDef *l_uart;
static bsp_uart_id_t l_uartId = BSP_UART_NONE;

static uint8_t l_rxByte;
static uint8_t l_rxRing[APP_CFG_MODEM_RX_RING_SIZE];
static volatile uint16_t l_rxHead;
static volatile uint16_t l_rxTail;
static volatile uint32_t l_overflow;
static volatile uint32_t l_errorCount;
static volatile uint32_t l_rxCount;
static uint8_t l_txRing[APP_CFG_MODEM_TX_RING_SIZE];
static volatile uint16_t l_txHead;
static volatile uint16_t l_txTail;
static volatile uint16_t l_txChunk;
static volatile bool l_txBusy;
static BspModemRxSink l_sink;

/*--------------------------------------------------------------------------*/
/* 实例解析                                                                  */
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

/*--------------------------------------------------------------------------*/
/* 发送：非阻塞入队 + 发送中断分块搬出                                        */
/*--------------------------------------------------------------------------*/
static void txKick(void) {
    uint16_t length;

    if (l_txBusy || (l_txHead == l_txTail) || (l_uart == (UART_HandleTypeDef *)0)) {
        return;
    }
    /* 本次只发「从尾部到环尾」这一段，回绕的剩余部分由 TxCplt 继续推进。 */
    length = (l_txHead > l_txTail) ? (uint16_t)(l_txHead - l_txTail)
                                   : (uint16_t)(APP_CFG_MODEM_TX_RING_SIZE - l_txTail);
    if (length > MODEM_TX_CHUNK_MAX) {
        length = MODEM_TX_CHUNK_MAX;
    }
    l_txChunk = length;
    l_txBusy = true;
    if (HAL_UART_Transmit_IT(l_uart, &l_txRing[l_txTail], length) != HAL_OK) {
        l_txBusy = false;   /* 启动失败：数据留在队列里，计数后下次再试 */
        ++l_errorCount;
    }
}

/** @brief 发送缓冲剩余空间（保留一个空位用于区分空/满）。 */
static uint16_t txFree(void) {
    uint16_t head = l_txHead;
    uint16_t tail = l_txTail;

    return (head >= tail) ? (uint16_t)(APP_CFG_MODEM_TX_RING_SIZE - 1U - (head - tail))
                          : (uint16_t)(tail - head - 1U);
}

static modem_port_status_t portWrite(void *context, uint8_t const *data, size_t len) {
    uint16_t head;
    size_t index;

    (void)context;
    if ((data == (uint8_t const *)0) || (len == 0U)) {
        return MODEM_PORT_ERROR;
    }
    /* 先问空间再整包入队：Port 契约要求「要么整包接受、要么 BUSY」，
     * 只收一半会让 AT 命令残缺，比直接返回忙更难排查。 */
    __disable_irq();
    if (len > txFree()) {
        __enable_irq();
        return MODEM_PORT_BUSY;
    }
    head = l_txHead;
    for (index = 0U; index < len; ++index) {
        l_txRing[head] = data[index];
        head = (uint16_t)((head + 1U) & (APP_CFG_MODEM_TX_RING_SIZE - 1U));
    }
    l_txHead = head;
    __enable_irq();
    txKick();
    return MODEM_PORT_OK;
}

/*--------------------------------------------------------------------------*/
/* 接收：上层读取（非阻塞）                                                  */
/*--------------------------------------------------------------------------*/
static size_t portRead(void *context, uint8_t *out, size_t cap) {
    size_t count = 0U;

    (void)context;
    if (l_uart == (UART_HandleTypeDef *)0) {
        return 0U;
    }
    /* 上电后或出错后接收链路可能没在跑，这里顺手把它拉起来，避免永久收不到数据。 */
    if (l_uart->RxState != HAL_UART_STATE_BUSY_RX) {
        __HAL_UART_CLEAR_OREFLAG(l_uart);
        __HAL_UART_FLUSH_DRREGISTER(l_uart);
        (void)HAL_UART_Receive_IT(l_uart, &l_rxByte, 1U);
    }
    while ((count < cap) && (l_rxTail != l_rxHead)) {
        out[count] = l_rxRing[l_rxTail];
        l_rxTail = (uint16_t)((l_rxTail + 1U) & (APP_CFG_MODEM_RX_RING_SIZE - 1U));
        ++count;
    }
    return count;
}

static uint32_t portTick(void *context) {
    (void)context;
    return HAL_GetTick();
}

static void portTrace(void *context, char const *text) {
    (void)context;
    (void)text;
}

/** @brief MN316_EN(PB9) 拉高：模组电源使能，高开启、低关闭。
*
* NOTE (为什么写在 BSP 里)：这是「上电时序」而不是业务逻辑，必须发生在装载接收之前；
*   同时 .ioc 里已把 PB9 登记成 GPIO_Output（初始 High），CubeMX 再生成后 gpio.c 会配置
*   同一引脚——本函数是幂等的，重复配置无害。
* NOTE (电平)：低电平会把模组断电，现象是收不到任何字节且 uart_err = 0，属最难判读的
*   静默故障，所以这里显式拉高而不是依赖复位默认值。
* NOTE (复位脚 NB_RESET/PB8)：按用户 2026-09-20 的决定**不配置**——三极管极性尚未确认，
*   配错会把模组一直摁在复位态。断电重启由电源脚 PB9 完成，不需要 PB8。 */
static void modemPowerEnable(void) {
    GPIO_InitTypeDef gpio = {0};

    gpio.Pin   = GPIO_PIN_9;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &gpio);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET);
}

/*..........................................................................*/
/**
* @brief 模组电源开关（`MN316_EN`，高开启 / 低关闭）。
*
* NOTE (非阻塞)：本函数只写电平并立即返回；「断电多久」由调用方（AO_Modem 的 20 ms
*   周期事件）计时——QV 协作式内核里绝不能在这里 HAL_Delay()。
* NOTE (为什么上电时重新配置引脚)：断电后再上电可能发生在别的路径上（将来换板可能
*   改由 BSP 全权管脚），重复 HAL_GPIO_Init() 是幂等的，代价只有几微秒。
*/
void BSP_modemPowerSet(bool on) {
    if (on) {
        modemPowerEnable();
    }
    else {
        HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_RESET);
    }
}

/*--------------------------------------------------------------------------*/
/* 初始化                                                                    */
/*--------------------------------------------------------------------------*/
void BSP_modemUartInit(ModemPort *port, bsp_uart_id_t uart) {
    if (port == (ModemPort *)0) {
        return;
    }
    l_uart = uartHandle(uart);
    if (l_uart == (UART_HandleTypeDef *)0) {
        return;   /* 编号非法：不配置外设，也不填 Port（调用方按未就绪处理） */
    }
    l_uartId = uart;
    l_rxHead = 0U;
    l_rxTail = 0U;
    l_txHead = 0U;
    l_txTail = 0U;
    l_txChunk = 0U;
    l_txBusy = false;
    l_overflow = 0U;
    l_errorCount = 0U;
    l_rxCount = 0U;
    /* NOTE (不要清 sink，2026-09-20)：sink 是 App 层的接线（App_start 里注册），不是
     * 本端口的内部状态。断电重启后 AO 会再调一次本函数重装接收，若在这里清掉 sink，
     * 中断就再也送不到 AO、表现为「模组有应答但状态机不动」——极难定位。 */
    modemPowerEnable();   /* 先给模组上电，再配置线速率与接收 */

    /* NOTE (波特率必须在这里真正写进外设，2026-09-27 修)：CubeMX 生成的
    * `MX_USART3_UART_Init()` 用的是 `.ioc` 里的值，而 `.ioc` **没有** `USART3.BaudRate`
    * （只有 `USART3.IPParameters=VirtualMode`），于是按默认值生成了 **115200**。
    * 原先本函数只解析实例、上电、装接收，**从未应用** `APP_CFG_MODEM_UART_BAUD`——
    * 后果是这个宏只被启动日志打印出来、从未作用到硬件：日志写 9600、实际跑 115200，
    * 模组收到乱码不应答，现象是「rx 为空、一个字都收不到、uart_err 还是 0」，
    * 与「模组没供电」完全同形，极难判读（2026-09-27 上板实测正是如此）。
    *
    * NOTE (为什么放这里而不是 .ioc)：本端口的实例是**装配信息**（BLE 与 4G 已经对调过一次），
    * 端口必须按自己的用途配置所占实例的线速率；把速率钉死在 `.ioc` 里，一旦实例对调
    * 就会有端口跑在错的速率上。`app_config.h` 的 APP_CFG_MODEM_UART_BAUD 才是唯一真相，
    * 启动日志打印的是**实际生效值**（BSP_modemUartBaud()）。 */
    l_uart->Init.BaudRate = (uint32_t)APP_CFG_MODEM_UART_BAUD;
    if (HAL_UART_Init(l_uart) != HAL_OK) {
        ++l_errorCount;   /* 配置失败：链路必然不通，由 AO 的失败日志暴露出来 */
    }

    (void)HAL_UART_Receive_IT(l_uart, &l_rxByte, 1U);
    port->context = l_uart;
    port->write = &portWrite;
    port->read = &portRead;
    port->get_tick_ms = &portTick;
    port->trace = &portTrace;
}

bool BSP_modemUartIsInstance(bsp_uart_id_t uart) {
    return (uart != BSP_UART_NONE) && (uart == l_uartId);
}

void BSP_modemUartSetSink(BspModemRxSink sink) {
    l_sink = sink;
}

/*--------------------------------------------------------------------------*/
/* 中断服务侧入口：只存字节、重装外设、通知 sink                              */
/*--------------------------------------------------------------------------*/
void BSP_modemUartRxComplete(void) {
    uint16_t next = (uint16_t)((l_rxHead + 1U) & (APP_CFG_MODEM_RX_RING_SIZE - 1U));

    if (next == l_rxTail) {
        ++l_overflow;          /* 队列满：静默丢弃新字节，绝不阻塞中断 */
    } else {
        l_rxRing[l_rxHead] = l_rxByte;
        l_rxHead = next;
    }
    ++l_rxCount;
    (void)HAL_UART_Receive_IT(l_uart, &l_rxByte, 1U);
    if (l_sink != (BspModemRxSink)0) {
        l_sink();              /* 只投事件，不做任何解析 */
    }
}

void BSP_modemUartTxComplete(void) {
    l_txTail = (uint16_t)((l_txTail + l_txChunk) & (APP_CFG_MODEM_TX_RING_SIZE - 1U));
    l_txBusy = false;
    txKick();                  /* 还有待发数据就继续推进 */
}

void BSP_modemUartError(void) {
    ++l_errorCount;
    __HAL_UART_CLEAR_OREFLAG(l_uart);
    __HAL_UART_FLUSH_DRREGISTER(l_uart);
    (void)HAL_UART_Receive_IT(l_uart, &l_rxByte, 1U);   /* 错误后恢复接收 */
}

uint32_t BSP_modemOverflowCount(void) {
    return l_overflow;
}

uint32_t BSP_modemErrorCount(void) {
    return l_errorCount;
}

uint32_t BSP_modemUartRxCount(void) {
    return l_rxCount;
}

uint32_t BSP_modemUartBaud(void) {
    /* 返回**实际写进外设**的速率（不是 app_config.h 的宏）：启动日志据此打印，
    * 这样「配置了但没生效」这类问题一眼可见，见 BSP_modemUartInit() 的 NOTE。 */
    return (l_uart != (UART_HandleTypeDef *)0) ? l_uart->Init.BaudRate : 0U;
}

#endif /* APP_CFG_MODEM_ENABLE != 0 */
