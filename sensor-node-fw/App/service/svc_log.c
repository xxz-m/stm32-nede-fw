/*============================================================================
* svc_log.c —— 服务层：分级日志实现（异步）
*
* 层次定位：Service 层。只做「格式化 + 入队」，不等待、不发送。
* 发送由 BSP 的环形队列 + USART1 发送中断完成（HAL_UART_Transmit_IT）。
*===========================================================================*/
#include "svc_log.h"
#include "bsp.h"

#include <stdarg.h>
#include <stdio.h>

/*--------------------------------------------------------------------------*/
/** @brief 等级前缀字符（单字符，节省输出宽度）。 */
static char const *lvlTag(svc_log_lvl_t lvl) {
    switch (lvl) {
        case SVC_LOG_ERROR: return "E";
        case SVC_LOG_WARN:  return "W";
        case SVC_LOG_INFO:  return "I";
        case SVC_LOG_DEBUG: return "D";
        default:            return "?";
    }
}

/*--------------------------------------------------------------------------*/
/**
* @brief 格式化并异步输出一条日志。
*
* 输出形式：`[<等级> <上电毫秒>] <正文>\r\n`
*
* 说明：
*   - 运行期再判一次等级，便于将来用变量动态调级；编译期已按
*     APP_CFG_LOG_LEVEL 消除掉更低的等级，所以这里的判断几乎不会命中
*   - 使用 vsnprintf（标准库，由工具链提供）；未使用浮点，不会引入
*     printf 浮点支持带来的体积
*/
void SVC_Log_Printf(svc_log_lvl_t lvl, char const *fmt, ...) {
    char buf[APP_CFG_LOG_FMT_BUF_SIZE];
    int n;
    va_list ap;

    if ((fmt == (char const *)0) || (lvl > SVC_LOG_DEBUG)) {
        return;
    }

    /* 前缀：等级 + 上电毫秒，便于与示波器/逻辑分析仪对齐时间 */
    n = snprintf(buf, sizeof(buf), "[%s %lu] ", lvlTag(lvl), (unsigned long)BSP_uptimeMs());
    if (n < 0) {
        return;
    }
    if ((size_t)n >= sizeof(buf)) {
        n = (int)(sizeof(buf) - 1U); /* 前缀异常长时截断，保证后续仍安全 */
    }

    va_start(ap, fmt);
    (void)vsnprintf(&buf[n], sizeof(buf) - (size_t)n, fmt, ap);
    va_end(ap);

    BSP_logWrite(buf);      /* 入队（非阻塞） */
    BSP_logWrite("\r\n");
}

/*--------------------------------------------------------------------------*/
void SVC_Log_Write(char const *str) {
    if (str != (char const *)0) {
        BSP_logWrite(str);
    }
}
