/* 服务层：寄存器负载校验和逐项编解码；标准C，与硬件和内核无关。 */
#ifndef SVC_REGISTER_H
#define SVC_REGISTER_H
#include <stdint.h>
#include <stdbool.h>
#include "app_config.h"

typedef enum {
    SVC_REG_OK, SVC_REG_XY, SVC_REG_XX, SVC_REG_RO, SVC_REG_OV,
    SVC_REG_IC, SVC_REG_NP, SVC_REG_SE, SVC_REG_NR
} SvcRegisterStatus;

typedef struct {
    void *context;
    SvcRegisterStatus (*read)(void *, uint8_t, uint16_t *);
    SvcRegisterStatus (*write)(void *, uint8_t, uint16_t);
} SvcRegisterPort;

typedef struct {
    bool frame_error;
    unsigned frame_code;
    bool all_ok;
    char data[APP_CFG_REGISTER_RESPONSE_SIZE];
} SvcRegisterResult;

/** @brief 校验整帧后逐项调用端口，失败帧不触发任何写入；结果归调用方持有。 */
void SVC_Register_Execute(bool write, char const *data,
                          SvcRegisterPort const *port, SvcRegisterResult *out);
#endif
