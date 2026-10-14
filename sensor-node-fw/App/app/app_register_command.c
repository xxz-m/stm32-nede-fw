/* App层：寄存器命令与通道响应适配；STM32F103 / QV，快照由调用AO持有。 */
#include "app_register.h"
#include <stdio.h>

bool App_Register_Dispatch(AppCommandRequest const *request,
                           AppRegisterSnapshot const *snapshot, AppCommandResult *result) {
    SvcRegisterResult parsed;
    SvcRegisterPort port;
    bool write;
    int n;
    if (request == NULL || result == NULL) { return false; }
    if (request->command.kind != SVC_AT_KIND_REGISTER_READ &&
        request->command.kind != SVC_AT_KIND_REGISTER_WRITE) { return false; }
    write = request->command.kind == SVC_AT_KIND_REGISTER_WRITE;
    port.context = (void *)snapshot; port.read = App_Register_Read; port.write = App_Register_Write;
    SVC_Register_Execute(write, request->command.value, &port, &parsed);
    result->status = parsed.all_ok ? APP_COMMAND_RESULT_DONE : APP_COMMAND_RESULT_ERROR;
    result->error_code = (uint8_t)parsed.frame_code;
    if (request->source == APP_COMMAND_SOURCE_MODEM) {
        n = snprintf(result->raw, sizeof(result->raw), "%s", parsed.data);
    } else if (parsed.frame_error) {
        n = snprintf(result->raw, sizeof(result->raw), "%s\r\nERROR\r\n", parsed.data);
    } else {
        n = snprintf(result->raw, sizeof(result->raw), "+%s:\"%s\"\r\n%s\r\n",
                       write ? "WRITE" : "READ", parsed.data, parsed.all_ok ? "OK" : "ERROR");
    }
    if (n < 0 || (size_t)n >= sizeof(result->raw)) { result->raw[0] = '\0'; return false; }
    return true;
}
