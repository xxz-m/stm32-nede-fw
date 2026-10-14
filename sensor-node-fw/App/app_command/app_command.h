/*============================================================================
* app_command.h —— 统一设备命令的请求与结果模型（可移植）
*
* 层次定位：可移植命令模型层（整个目录搬走即可复用）。
*           只依赖标准 C 与 Service 层的 `svc_at.h`，**不访问** EEPROM、传感器、
*           通信模组、QP 或本工程任何 App 层接口。
*
* 目标硬件：与硬件无关（本文件不含任何硬件假设）
* 所属内核：不依赖框架
*
* 职责：描述「命令从哪来、命令是什么、动作结果怎么表示」这三件事。
*   具体动作由 `App/app/command_dispatch.c` 执行——这样串口与 4G 能共用同一套命令语义
*   和同一串 AT 响应文本，而命令模型本身保持与硬件解耦。
*
* 数据流：
*
*   串口 USART1 → svc_at.c → SvcAtCmd ─┐
*                                      ├→ AppCommandRequest → App_Command_Dispatch()
*   4G MQTT JSON {id,command} → 解析 ─┘                              ↓
*                                                        AppCommandResult → 回复
*
* NOTE (为什么 enum 与 struct 都要用)：`enum` 表达**有限的类别**（来源只有串口/4G，
*   结果只有完成/进行中/失败），`struct` 承载**一次具体事件**（这一条命令的解析结果、
*   这一条命令的关联 ID）。两者不混用，避免出现「用结构体枚举来源」或
*   「用枚举携带字符串」这类别扭写法。
*===========================================================================*/
#ifndef APP_COMMAND_H
#define APP_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

#include "svc_at.h"

/*! 4G 下行关联 ID 的最大保存长度（含结尾 '\0'）。
* 上位机的关联 ID 是短字符串（如 "42"），64 字节对当前协议足够；
* 4G 侧拿到超长 ID 时应整条丢弃并记录日志，不要截断后回一个对不上的 ID。 */
#define APP_COMMAND_CORRELATION_MAX  (64U)

/** @brief 命令来源：决定「谁发的」以及「结果往哪里回」。 */
typedef enum {
    APP_COMMAND_SOURCE_SERIAL = 0,  /*!< 调试串口 USART1：结果直接写异步日志队列 */
    APP_COMMAND_SOURCE_MODEM        /*!< 4G MQTT 下行：结果要包成 `command/reply` JSON */
} AppCommandSource;

/**
 * @brief 一条待执行的统一命令请求。
 *
 * @note `command` 是 Service 层解析器的输出，按值保存（结构体很小），
 *       因此请求的生命周期完全由调用方掌握，不需要跨 AO 传指针。
 */
typedef struct {
    AppCommandSource source;        /*!< 谁发起的 */
    SvcAtCmd         command;       /*!< 解析后的命令（kind / param / value） */
    bool             has_correlation;  /*!< 是否有云端关联 ID（串口恒为 false） */
    char             correlation[APP_COMMAND_CORRELATION_MAX];  /*!< 4G 的 `id`，原样回填 */
} AppCommandRequest;

/** @brief 命令的完成状态。 */
typedef enum {
    APP_COMMAND_RESULT_DONE = 0,  /*!< 已执行完并生成响应文本 */
    APP_COMMAND_RESULT_PENDING,   /*!< 已受理、异步完成中（预留；当前没有命令返回它） */
    APP_COMMAND_RESULT_ERROR      /*!< 执行失败，`raw` 里是错误响应 */
} AppCommandResultStatus;

/**
 * @brief 一条统一命令的执行结果。
 *
 * @note raw按来源适配：串口为完整AT响应；云端寄存器为纯负载或帧错误文本，
 *       基础AT/RST为含CRLF的结果行。组件仅将raw作为JSON的data转义。
 *       容量取SVC_AT_RESP_MAX，必须能容纳132字符寄存器回复及串口封装。
 */
typedef struct {
    AppCommandResultStatus status;      /*!< 完成 / 进行中 / 失败 */
    uint16_t               error_code;  /*!< 设备侧错误码，供 4G 回包透传；成功为 0 */
    char                   raw[SVC_AT_RESP_MAX];  /*!< 完整 AT 响应文本（含 `\r\n`） */
} AppCommandResult;

#endif /* APP_COMMAND_H */
