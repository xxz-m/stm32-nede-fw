/*============================================================================
* modem_port.h —— 4G 组件唯一平台端口
* 层次定位：Component 层；平台通过函数表注入字节流与毫秒时基。
* 约束    ：本头文件只依赖标准 C，不得出现 HAL / CMSIS / QP / RTOS 类型。
*
* 四个回调都在调用者上下文执行：组件自身不创建线程，也不进入中断。
*   - write()       非阻塞，只能「整段接受」或返回 MODEM_PORT_BUSY/ERROR，
*                   禁止只接受前半段就返回 OK（会让 AT 命令残缺）。
*   - read()        从接收环形缓冲取走当前已有字节，最多 capacity 个；
*                   没有数据立即返回 0。
*   - get_tick_ms() 单调递增的 32 位毫秒值，允许自然回绕。
*   - trace()       只接收已格式化好的 ASCII 文本，可为 NULL。
*===========================================================================*/
#ifndef MODEM_PORT_H
#define MODEM_PORT_H

#include <stddef.h>
#include <stdint.h>

typedef enum {
    MODEM_PORT_OK = 0,  /* 成功                 */
    MODEM_PORT_BUSY,    /* 暂时无空间，稍后重试 */
    MODEM_PORT_ERROR    /* 参数或链路错误       */
} modem_port_status_t;

typedef struct {
    void *context;
    modem_port_status_t (*write)(void *, uint8_t const *, size_t);
    size_t (*read)(void *, uint8_t *, size_t);
    uint32_t (*get_tick_ms)(void *);
    void (*trace)(void *, char const *);
} ModemPort;

#endif /* MODEM_PORT_H */
