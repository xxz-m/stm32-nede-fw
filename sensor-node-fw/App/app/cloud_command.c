/* App层：云端仅开放AT/RST/寄存器，避免诊断写命令从云端绕过只读保护；STM32/QV。 */
#include "cloud_command.h"
#include "command_dispatch.h"
#include <string.h>
void App_CloudCommand_Dispatch(struct ModemCommandRequest const *wire,
                               AppRegisterSnapshot const *snapshot, AppCommandResult *result) {
    AppCommandRequest request;
    bool handled = false;
    memset(&request, 0, sizeof(request));
    memset(result, 0, sizeof(*result));
    request.source = APP_COMMAND_SOURCE_MODEM;
    memcpy(request.correlation, wire->id, strlen(wire->id) + 1U);
    request.has_correlation = true;
    if (strcmp(wire->command, "AT+READ") == 0 || strcmp(wire->command, "AT+WRITE") == 0) {
        request.command.kind = strcmp(wire->command, "AT+WRITE") == 0 ?
                               SVC_AT_KIND_REGISTER_WRITE : SVC_AT_KIND_REGISTER_READ;
        memcpy(request.command.value, wire->data, strlen(wire->data) + 1U);
        handled = App_Register_Dispatch(&request, snapshot, result);
    } else if (!wire->has_data && (strcmp(wire->command, "AT") == 0 || strcmp(wire->command, "AT+RST") == 0)) {
        request.command.kind = strcmp(wire->command, "AT") == 0 ? SVC_AT_KIND_PING : SVC_AT_KIND_REBOOT;
        handled = App_Command_Dispatch(&request, result);
    }
    if (!handled) {
        result->status = APP_COMMAND_RESULT_ERROR;
        memcpy(result->raw, "ERROR\r\n", sizeof("ERROR\r\n"));
    }
}
