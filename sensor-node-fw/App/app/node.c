/*============================================================================
* node.c —— 应用层：AO_Node 实现
*
* 层次定位：App 层。
*
* 状态机（单状态）：
*
*   Node_state_Active —— 只做两件事：
*     1) 抽干 BSP 环形缓冲里的接收字节，按行交给 AT 解析器；
*     2) 把解析出的命令交给 atExecute() 生成响应，写进异步日志队列。
*
* 命令语法与响应形态见 svc_at.h 的文件头与 docs_sensor/06-协议格式.md。
* 跨 AO 交互统一用全局段信号（SYS_SENSOR_*），并投递到 AO_Sensor 的队列。
*===========================================================================*/
#include "node.h"
#include "app_signals.h"
#include "app_prio.h"
#include "app_config.h"
#include "bsp.h"
#include "sensor.h"
#include "store.h"
#include "svc_log.h"

#include <stdio.h>
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

/*..........................................................................*/
/**
* @brief 把 AT 名转成大写写进 out（响应里的参数名一律大写，3GPP 惯例）。
* @return false = 放不下（调用方按 ERROR 处理）
*/
static bool nameUpper(char *out, size_t cap, char const *name) {
    size_t i;

    for (i = 0U; name[i] != '\0'; ++i) {
        if ((i + 1U) >= cap) {
            return false;
        }
        out[i] = ((name[i] >= 'a') && (name[i] <= 'z'))
                 ? (char)(name[i] - 'a' + 'A') : name[i];
    }
    out[i] = '\0';
    return true;
}

/*..........................................................................*/
/**
* @brief 执行一条已解析的 AT 命令并生成响应文本。
*
* NOTE (分层)：解析在 Service 层（svc_at.c），执行与回复在 App 层。
*   参数写命令只更新内存影子并置 dirty，真正的落盘由 AO_Store 异步完成，
*   因此这里**不会阻塞**——回 `OK` 表示「已受理」，落盘结果随后由日志给出。
*
* NOTE (响应形态，2026-09-27 改写)：按 V.250 / 3GPP 27.007 的常规形式：
*   - 成功一律大写 `OK`（原先回小写 `ok`，与标准不符）
*   - 读参数回 `+<NAME>:"<值>"`（字符串参数带引号）
*   - 测试形式回 `+<NAME>:(<最大长度>)`
*   - 查状态回**位置式** `+CFG:<8 个逗号分隔字段>`（原先回 `+cfg:key=value` 自造格式）
*   - 失败回 `+CME ERROR:<n>`（原先回自造的 `+err:<n>`）
*
* @return true = 已生成响应（resp 有效）；false = 该命令在本层无人处理
*/
static bool atExecute(SvcAtCmd const *cmd, char *resp, size_t cap) {
    int16_t id;

    switch (cmd->kind) {
        case SVC_AT_KIND_PING:
            return replyText(resp, cap, "OK\r\n");

        case SVC_AT_KIND_GET: {
            char name[SVC_AT_NAME_MAX];

            id = SVC_Param_Find(cmd->param);
            if (id < 0) {
                return replyText(resp, cap, "+CME ERROR:1\r\n");  /* 参数名不认识 */
            }
            if (!nameUpper(name, sizeof(name), SVC_Param_Name((uint8_t)id))) {
                return false;
            }
            (void)snprintf(resp, cap, "+%s:\"%s\"\r\n",
                           name, App_Store_Value((uint8_t)id));
            return true;
        }

        case SVC_AT_KIND_TEST: {
            char name[SVC_AT_NAME_MAX];

            id = SVC_Param_Find(cmd->param);
            if (id < 0) {
                return replyText(resp, cap, "+CME ERROR:1\r\n");
            }
            if (!nameUpper(name, sizeof(name), SVC_Param_Name((uint8_t)id))) {
                return false;
            }
            /* 容量含结尾 '\0'，因此可写的最大字符数是 cap - 1 */
            (void)snprintf(resp, cap, "+%s:(%u)\r\n",
                           name, (unsigned)(SVC_Param_Cap((uint8_t)id) - 1U));
            return true;
        }

        case SVC_AT_KIND_SET: {
            switch (App_Store_SetName(cmd->param, cmd->value)) {
                case SVC_OK:        return replyText(resp, cap, "OK\r\n");
                case SVC_ERR_PARAM: return replyText(resp, cap, "+CME ERROR:1\r\n");
                case SVC_ERR_RANGE: return replyText(resp, cap, "+CME ERROR:2\r\n");
                case SVC_ERR_STATE: return replyText(resp, cap, "+CME ERROR:3\r\n");
                default:            return replyText(resp, cap, "ERROR\r\n");
            }
        }

        case SVC_AT_KIND_INFO: {
            AppStoreInfo info;
            App_Store_Info(&info);
            /* 字段顺序固定：rdy,present,loaded,dirty,ver,seq,ok,fail（只许在表尾追加） */
            (void)snprintf(resp, cap, "+CFG:%u,%u,%u,%u,%u,%u,%lu,%lu\r\n",
                           (unsigned)(info.resolved ? 1U : 0U),
                           (unsigned)(info.present ? 1U : 0U),
                           (unsigned)(info.loaded ? 1U : 0U),
                           (unsigned)(info.dirty ? 1U : 0U),
                           (unsigned)info.config_version,
                           (unsigned)info.slot_seq,
                           (unsigned long)info.saved,
                           (unsigned long)info.failed);
            return true;
        }

        case SVC_AT_KIND_DEFAULTS:
            if (!App_Store_ResetDefaults()) {
                return replyText(resp, cap, "+CME ERROR:3\r\n");   /* 存储还没就绪 */
            }
            return replyText(resp, cap, "OK\r\n");
				case SVC_AT_KIND_VERSION:{
						(void)snprintf(resp,cap,"+VER:\"%s\",\"%s\"\r\n",APP_CFG_FW_VERSION, APP_CFG_FW_BUILD_DATE);
						return true;
				}
        case SVC_AT_KIND_SCAN:
            /* 扫描结果不走本响应：它要跨十几拍总线，由 AO_Store 逐条以
            * `+I2C:<地址>` 日志上报（见 store.c），这里只回「已受理」。 */
            if (!App_Store_RequestScan()) {
                return replyText(resp, cap, "+CME ERROR:5\r\n");   /* 已有扫描在跑 / 未就绪 */
            }
            return replyText(resp, cap, "OK\r\n");

        default:
            return false;
    }
}

/*..........................................................................*/
/**
* @brief 行结束处理：整行交给 AT 解析器；不是 AT 命令则按 AT 惯例回 ERROR。
*
* NOTE (设计)：AT 指令按整行判定，匹配时只回响应，不回显输入字符；
* 非 AT 输入在行结束时统一回 ERROR，避免逐字符回显把 AT 的 'A' 也打出来。
*
* @return true 表示该行被 AT 命令消费（已回复，不再回 ERROR）
*/
static bool atHandleLine(Node * const me, uint8_t term) {
    bool handled = false;
    char resp[SVC_AT_RESP_MAX];
    SvcAtCmd cmd;
    uint8_t i;

    for (i = 0U; i < me->lineLen; ++i) {
        if (SVC_At_Feed(&me->at, (uint8_t)me->lineBuf[i], &cmd) &&
            atExecute(&cmd, resp, sizeof(resp))) {
            LOG_WRITE(resp);
            handled = true;
        }
    }
    if (SVC_At_Feed(&me->at, term, &cmd) && atExecute(&cmd, resp, sizeof(resp))) {
        LOG_WRITE(resp);
        handled = true;
    }

    if (!handled) {
        /* 不是 AT 指令：按 AT 惯例回 ERROR（空行静默，避免刷屏） */
        if (me->lineLen > 0U) {
            LOG_WRITE("ERROR\r\n");
        }
    }
    me->lineLen = 0U;

    return handled;
}

/*..........................................................................*/
/**
* @brief 处理一个接收字节。
*
* NOTE (设计)：调试串口只支持 AT 指令（见 06-协议格式.md）——
*   行结束符   → 整行判定 AT：命中回 ok，未命中回 ERROR；
*   可打印字符 → 存入行缓冲，等行结束统一处理；
*   其他控制字节 → 直接回 ERROR（不解析、不回显）。
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
            BSP_printLine("names  : SN NAME PIN UUID DEVNAME MODEL BATCH");
            BSP_printLine("store  : AT+VER? |AT+CFG? status | AT&F defaults | AT+I2C? scan");
					
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
