/* App层：云端命令白名单与寄存器适配；STM32F103/QV，不访问硬件。 */
#ifndef APP_CLOUD_COMMAND_H
#define APP_CLOUD_COMMAND_H
#include "app_register.h"
#include "modem_component.h"
/** @brief 分发完整云端请求，所有路径均生成可发送的结果。 */
void App_CloudCommand_Dispatch(struct ModemCommandRequest const *wire,
                               AppRegisterSnapshot const *snapshot, AppCommandResult *result);
#endif
