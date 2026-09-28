/*============================================================================
* command_dispatch.h —— App 层：统一设备命令分发接口
*
* 层次定位：**App(应用层)**。声明「统一命令 → 本工程动作」的唯一入口。
*
* 目标硬件：STM32F103RCT6（LQFP64）
* 所属内核：QP/C 6.9.1 · QV（协作式、非抢占、单栈）
*
* 调用点（两个来源共用同一个入口，这正是本模块存在的目的）：
*   - `App/app/node.c`：串口 AT 命令，`source = APP_COMMAND_SOURCE_SERIAL`
*   - 后续 4G：MQTT `command` 下行，`source = APP_COMMAND_SOURCE_MODEM`
*
* 实现在 `App/app/command_dispatch.c`，那里有数据流图、响应形态与错误码的完整说明。
*
* NOTE (为什么接口留在 App 层，而不是放进 `App/app_command/`)：`App/app_command/` 只放
*   可整体搬走的命令模型（请求/结果结构）；执行动作要访问 `App_Store_*`，属于本工程业务，
*   所以留在 App 层。这样命令模型能被别的工程复用，而硬件相关动作不会被一起带过去。
*
* NOTE (调用约束)：只允许在 AO 的状态处理函数（QV 单栈、运行到完成）里调用，
*   **不允许从中断调用**。函数本身没有静态状态，结果写在调用方传入的 `result` 上。
*===========================================================================*/
#ifndef COMMAND_DISPATCH_H
#define COMMAND_DISPATCH_H

#include <stdbool.h>

#include "app_command.h"

/**
 * @brief 执行一条统一设备命令（串口与 4G 共用）。
 *
 * @param request 输入：统一命令请求（来源 + `SvcAtCmd` + 可选关联 ID），不得为 NULL。
 * @param result  输出：统一命令结果（状态 + 错误码 + AT 原始响应），不得为 NULL。
 * @return true  = 命令已识别并生成结果（**执行失败同样是 true**，此时看 `result->status`）；
 *         false = 本分发器不处理该命令，或入参为空指针，由调用方自行兜底。
 *
 * @note 返回 `true` 不代表执行成功，只代表「这条命令归本模块管」。
 *       串口调用方应输出 `result.raw`（已含行尾 `\r\n`）；
 *       4G 调用方应把 `result.raw` 当作 `data` 包进 `command/reply` JSON。
 */
bool App_Command_Dispatch(AppCommandRequest const *request,
                          AppCommandResult *result);

#endif /* COMMAND_DISPATCH_H */
