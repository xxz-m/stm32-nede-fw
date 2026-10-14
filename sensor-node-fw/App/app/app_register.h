/* App层：寄存器设备语义与调用方持有的测量快照；STM32F103 / QV。 */
#ifndef APP_REGISTER_H
#define APP_REGISTER_H
#include "app_command.h"
#include "svc_register.h"

typedef struct {
    int32_t angle_cdeg[3];
    int32_t acc_mg[3];
    int32_t temperature_cC;
    uint16_t battery_mV;
    bool valid;
    bool battery_valid;
} AppRegisterSnapshot;

/** @brief 映射一个设备寄存器；context为所属AO的只读同批快照，可为NULL。 */
SvcRegisterStatus App_Register_Read(void *context, uint8_t address, uint16_t *value);
/** @brief 写入已开放的周期参数；模式/掩码在本阶段拒写，绝不虚报生效。 */
SvcRegisterStatus App_Register_Write(void *context, uint8_t address, uint16_t value);
/** @brief 统一串口/云端寄存器语义，回复按来源选用AT文本或纯负载。 */
bool App_Register_Dispatch(AppCommandRequest const *request,
                           AppRegisterSnapshot const *snapshot, AppCommandResult *result);
#endif
