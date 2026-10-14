/*============================================================================
* node.h —— 应用层：AO_Node（调试控制台）
*
* 层次定位：**App(应用层)** → Service → Driver → BSP → HAL/CMSIS
*           应用层与硬件无关，只能调用下层暴露的接口。
*
* 职责：
*   - 调试串口 AT 命令行（标准 AT 形态，见 svc_at.h 与 06-协议格式.md）：
*     `AT` 握手、`AT+<参数名>?` 读、`AT+<参数名>="<值>"` 写、`AT+<参数名>=?` 查最大长度、
*     `AT+CFG?` 存储状态、`AT+VER?` 查询版本、`AT&F` 恢复默认值、`AT+I2C?` 扫描总线
*     （解析在 Service 层 svc_at.c，执行与回复在本文件的 App_Command_Dispatch()）
*   - 「中断 → 事件」通路的接收端：USART1 中断通过 Node_uartRxIsrSink()
*     把字节放进 BSP 环形缓冲并投递 NODE_RX_BYTE_SIG
*
* NOTE (2026-09-27 接口改写)：原先的 `at`/`sn?`/`sn=值`/`cfg?`/`cfg!`/`i2c?` 自定义语法与
*   裸 `at` 捷径已删除，不再兼容旧写法；响应统一为 `OK` / `+<NAME>:"<值>"` / `+CME ERROR:<n>`。
*
* 信号定义集中在 App/app/app_signals.h 的 Node 段（不再使用本地 MAX_SIG）。
* 状态机：单状态 Active（原先的 Boot/Running/Halted 与单字符命令已删除）。
*
* NOTE (心跳)：本 AO **不再**做周期心跳——1 Hz 采样与 2 s 心跳分别由
*   AO_Sensor 与 AO_Heartbeat 负责，Node 只处理控制台输入。
*===========================================================================*/
#ifndef NODE_H
#define NODE_H

#include "qpc.h"
#include "svc_at.h"
#include "app_register.h"

/*! 采样结果按值发送，串口AO独立持有寄存器快照。 */
typedef struct { QEvt super; AppRegisterSnapshot snapshot; } NodeMeasurementEvt;

/* Node 活动对象 ------------------------------------------------------------*/
typedef struct {
    QActive  super;   /* 基类，必须放在首位        */
    QTimeEvt timeEvt; /* 预留：本 AO 当前不用周期事件 */
    SvcAt    at;      /* AT 指令解析状态（Service 层，状态由 AO 持有） */
    AppRegisterSnapshot registers;
    char     lineBuf[SVC_AT_LINE_MAX + 1U]; /* 控制台行缓冲：整行判定用（见 06-协议格式） */
    uint8_t  lineLen; /* 行缓冲中已存字节数 */
    bool lineOverflow; /* 超长整行拒绝，不能执行截断后的前缀。 */
    uint32_t seq;     /* 预留计数                  */
    uint32_t cmds;    /* 已处理的接收字节数        */
} Node;

/* AO 对外只暴露三样：构造函数、启动函数、单例句柄 --------------------------*/

/*! 构造 AO（只做对象初始化，不启动）。 */
void Node_ctor(void);

/*! 启动 AO：事件队列存储由本模块自己持有并交给 QP 内核。 */
void Node_start(void);

/*! 单例句柄。 */
extern QActive * const AO_Node;

/*! USART1 中断的接收端（由 App 装配点用 BSP_uartRxSetSink() 注册）。
* @note 在中断上下文运行：只做「存数据 + 投递事件」，见 07-接口约定。
*/
void Node_uartRxIsrSink(uint8_t byte);

#endif /* NODE_H */
