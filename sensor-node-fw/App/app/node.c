/*============================================================================
* node.c —— 应用层：AO_Node 实现
*
* 层次定位：App 层。
*
* 目标硬件：STM32F103RCT6（LQFP64）· 调试口 USART1（PA9/PA10，115200 8N1）
* 所属内核：QP/C 6.9.1 · QV（协作式、非抢占、单栈）
*
* 状态机（单状态）：
*
*   Node_state_Active —— 只做两件事：
*     1) 抽干 BSP 环形缓冲里的接收字节，按行交给 AT 解析器；
*     2) 把解析出的命令交给 App_Command_Dispatch() 生成响应，写进异步日志队列。
*
* NOTE (职责边界)：本文件是**串口通道**，不是命令实现。它只负责「收字节 → 解析 →
*   包成 AppCommandRequest → 输出 AppCommandResult.raw」，不认识任何具体命令。
*   命令语义与响应格式在 App/app/command_dispatch.c；新增命令请改那里。
*
* 命令语法见 svc_at.h 的文件头，响应形态见 docs_sensor/06-协议格式.md。
* 跨 AO 交互统一用全局段信号（SYS_SENSOR_*），并投递到 AO_Sensor 的队列。
*===========================================================================*/
#include "node.h"
#include "app_signals.h"
#include "app_prio.h"
#include "bsp.h"
#include "svc_log.h"
#include "app_command.h"
#include "command_dispatch.h"

#include <string.h>

/* 本模块在 QP 断言里的名字 */
Q_DEFINE_THIS_MODULE("node")

/* QP/C 断言钩子（实现在 App/bsp/bsp.c） */
void Q_onAssert(char const * const module, int loc);

/* 状态处理函数前置声明：Q_TRAN() 用到的符号必须先可见 */
QState Node_state_Active(Node * const me, QEvt const * const e);

/* AO 实例与事件队列（队列存储归 AO 自己所有，整改项 5） */
static Node l_node;
QActive * const AO_Node = &l_node.super;
static QEvt const *l_nodeQueueSto[10];

/* 事件 ---------------------------------------------------------------------*/
/* 中断投递用的静态事件：只表示「环形缓冲里有数据了」，字节在 BSP 缓冲里 */
static QEvt const l_rxByteEvt = { (QSignal)NODE_RX_BYTE_SIG, 0U, 0U };

/* 说明：发往 AO_Sensor 的静态事件已移出本模块——采样改为上电常开，
 * 由 App/app/app.c 的装配点（App_start）投递 SYS_SENSOR_START_SIG。 */

/*--------------------------------------------------------------------------*/
/* 本文件内部辅助函数                                                        */
/*--------------------------------------------------------------------------*/
/* 控制台不再提供单字符命令（g/h/s/x/d/? 已删除）：调试串口只走 AT 指令，
 * 见 docs_sensor/06-协议格式.md。 */

/*..........................................................................*/
/* 说明：不可打印字节的十六进制回显（echoByteHex）已随单字符控制台一并删除——
 * AT-only 模式下，非 AT 输入统一回 ERROR，不再回显收到的字节。 */

/**
* @brief 把 1 个可打印字符追加进行缓冲（超出上限的部分丢弃）。 */
static void linePut(Node * const me, uint8_t b) {
    if (me->lineLen < (uint8_t)SVC_AT_LINE_MAX) {
        me->lineBuf[me->lineLen] = (char)b;
        ++me->lineLen;
    }
}

/*..........................................................................*/
/** @brief 把一段文本（含 CRLF）写入响应缓冲。
* @return false = 缓冲放不下（此时不回复，避免截断成半条响应） */
static bool replyText(char *resp, size_t cap, char const *text) {
    size_t const len = strlen(text);

    if (cap <= len) {
        return false;
    }
    (void)memcpy(resp, text, len + 1U);
    return true;
}

/*--------------------------------------------------------------------------*/
/**
 * @brief 统一执行一条已经解析的串口 AT 命令。
 *
 * @param cmd 已解析的命令（来自 Service 层的 `SVC_At_Feed()`）
 * @param resp 出参：响应缓冲（由本文件在栈上给出）
 * @param cap  resp 的容量
 * @return true = 已生成响应；false = 无人处理该命令（调用方按 ERROR 处理）
 *
 * NOTE (分层)：本函数只做「串口 → 统一模型 → 结果 → 串口」的搬运，**不认识任何具体命令**，
 *   只负责把来源标成 `APP_COMMAND_SOURCE_SERIAL`。命令语义、响应形态与错误码全部在
 *   `App/app/command_dispatch.c`；新增命令请改那里，**不要在本文件加 case**
 *   （协议表见 docs_sensor/06-协议格式.md）。
 */
static bool executeCommand(SvcAtCmd const *cmd,
                           char *resp,
                           size_t cap) {
    AppCommandRequest request;
    AppCommandResult result;

    if ((cmd == (SvcAtCmd const *)0) ||
        (resp == (char *)0)) {
        return false;
    }

    (void)memset(&request, 0, sizeof(request));
    request.source = APP_COMMAND_SOURCE_SERIAL;
    request.command = *cmd;
    request.has_correlation = false;

    if (!App_Command_Dispatch(&request, &result)) {
        return false;
    }

    if (result.raw[0] == '\0') {
        return false;
    }

    return replyText(resp, cap, result.raw);
}

/*..........................................................................*/
/**
 * @brief 行结束处理：整行交给 AT 解析器；不是 AT 命令则按 AT 惯例回 ERROR。
 *
 * @return true 表示已生成响应；false 表示本行未被命令分发器处理。
 */
static bool atHandleLine(Node * const me, uint8_t term) {
    bool handled = false;
    char resp[SVC_AT_RESP_MAX];
    SvcAtCmd cmd;
    uint8_t i;

    for (i = 0U; i < me->lineLen; ++i) {
        if (SVC_At_Feed(&me->at, (uint8_t)me->lineBuf[i], &cmd) &&
            executeCommand(&cmd, resp, sizeof(resp))) {
            LOG_WRITE(resp);
            handled = true;
        }
    }

    if (SVC_At_Feed(&me->at, term, &cmd) &&
        executeCommand(&cmd, resp, sizeof(resp))) {
        LOG_WRITE(resp);
        handled = true;
    }

    if (!handled && (me->lineLen > 0U)) {
        LOG_WRITE("ERROR\r\n");
    }

    me->lineLen = 0U;
    return handled;
}

/*..........................................................................*/
/**
 * @brief 处理一个接收字节。
 *
 * 行结束符交给 AT 解析器；可打印字符进入行缓冲；
 * 其它控制字节直接返回 ERROR。
 */
static void handleByte(Node * const me, uint8_t b) {
    if ((b == (uint8_t)'\r') || (b == (uint8_t)'\n')) {
        (void)atHandleLine(me, b);
        return;
    }

    if ((b >= 0x20U) && (b < 0x7FU)) {
        linePut(me, b);
        return;
    }

    LOG_WRITE("ERROR\r\n");
}

/*--------------------------------------------------------------------------*/
/* 中断上下文接口                                                            */
/*--------------------------------------------------------------------------*/
/**
* @brief USART1 中断的接收端（由 App 装配点注册给 BSP）。
*
* 只做三件事：存数据、投递事件、返回。环形缓冲由 BSP 拥有。
*/
void Node_uartRxIsrSink(uint8_t byte) {
    BSP_uartRxPut(byte);                       /* 1) 存数据        */
    (void)QACTIVE_POST_X(AO_Node, &l_rxByteEvt, 0U, 0U); /* 2) 投递事件 */
}

/*--------------------------------------------------------------------------*/
/* 状态处理函数                                                              */
/*--------------------------------------------------------------------------*/

/*..........................................................................*/
/**
* @brief Active：本 AO 的唯一状态（AT 通道 + 接收抽干）。
*
* NOTE (设计)：单字符命令与 Boot/Running/Halted 三态已删除——调试串口只走 AT 指令
* （见 06-协议格式.md）；心跳由独立 AO_Heartbeat 负责，本 AO 不再需要状态跳转。
*/
QState Node_state_Active(Node * const me, QEvt const * const e) {
    QState status;

    switch (e->sig) {
        case Q_ENTRY_SIG: {
            /* 参数命令的自助提示（ASCII English，见 AGENTS.md 5.2）。
            * 这几行是现场唯一的「命令清单」，改命令时同步更新它们与
            * docs_sensor/06-协议格式.md。 */
            BSP_printLine("at console ready (line-based AT, CR/LF terminated)");
            BSP_printLine("params : AT+SN? read | AT+SN=\"<v>\" write | AT+SN=? max len");
            BSP_printLine("names  : SN PIN UUID DEVNAME MODEL BATCH (BLE adv name = SN)");
            BSP_printLine("biz    : AT+TRANSINTVL? read | AT+TRANSINTVL=<1..20> min (saved)");
            BSP_printLine("store  : AT+CMD? | AT+VER? | AT+CFG? status | AT&F defaults | AT+I2C? scan");
            BSP_printLine("system : AT+RST | AT+REBOOT (reply OK first, then reset)");
					
            status = Q_HANDLED();
            break;
        }
        case NODE_RX_BYTE_SIG: {
            uint8_t b;
            status = Q_HANDLED();
            while (BSP_uartRxGet(&b)) {
                ++me->cmds;
                handleByte(me, b);   /* AT-only：不再有单字符命令 */
            }
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/*..........................................................................*/
QState Node_initial(Node * const me, void const * const par) {
    (void)me;
    (void)par;

    return Q_TRAN(&Node_state_Active);
}

/*--------------------------------------------------------------------------*/
/**
* @brief 构造 AO_Node。
*/
void Node_ctor(void) {
    Node *me = (Node *)AO_Node;

    QActive_ctor(&me->super, Q_STATE_CAST(&Node_initial));
    /* 说明：本 AO 不再使用周期时间事件——1 Hz/2 s 心跳输出由独立的 AO_Heartbeat
     * 负责，Node 只做 AT 通道与接收抽干（见 05-任务说明.md）。 */
    SVC_At_Init(&me->at);   /* AT 解析状态（Service 层，状态由本 AO 持有） */
    me->lineLen = 0U;       /* 控制台行缓冲清零 */
    me->seq  = 0U;
    me->cmds = 0U;
}

/*..........................................................................*/
/**
* @brief 启动 AO_Node（队列存储随本模块走，BSP 不再参与装配）。
*/
void Node_start(void) {
    QACTIVE_START(
        AO_Node,
        APP_NODE_PRIO,          /* 优先级来自 App 层装配表 */
        l_nodeQueueSto,         /* 队列存储属于 AO 自己     */
        Q_DIM(l_nodeQueueSto),
        (void *)0, 0U,          /* QV 内核不需要私有栈      */
        (QEvt *)0               /* 无初始化事件             */
    );
}
