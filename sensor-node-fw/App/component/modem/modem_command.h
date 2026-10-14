/* Component内部：云命令JSON编解码；标准C，端口和业务仍由公共组件头暴露。 */
#ifndef MODEM_COMMAND_H
#define MODEM_COMMAND_H
#include "modem_component.h"
/** @brief 严格读取平面字符串对象，拒绝重复键、截断及控制字符；无硬件副作用。 */
bool Modem_Command_Parse(char const *json, struct ModemCommandRequest *request);
/** @brief 寄存器回复仅id/data，基础AT回复回显command；容量不足不发布截断JSON。 */
bool Modem_Command_Format(char *out, size_t capacity, char const *id,
                           char const *command, char const *data);
#endif
