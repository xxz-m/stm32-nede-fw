/*============================================================================
* svc_log.h —— 服务层：分级日志（异步发送）
*
* 层次定位：App(应用层) → **Service(服务层)** → BSP → HAL/CMSIS
*
* 数据通路（全异步，不阻塞调用者）：
*
*   LOG_x(fmt, ...)  宏（编译期按 APP_CFG_LOG_ENABLE / LEVEL 消除）
*        -> SVC_Log_Printf()  用 vsnprintf 格式化成一条文本
*        -> BSP_logWrite()    写入 BSP 的发送环形队列（FIFO，满则按配置丢弃）
*        -> USART1 发送中断   逐块搬出（HAL_UART_Transmit_IT + TxCplt 回调）
*
* 约定：
*   - 关闭日志时 `LOG_x(...)` 展开为 `((void)0)`，**调用点不需要写 if**
*   - 低于 APP_CFG_LOG_LEVEL 的等级在编译期消除，运行时零开销
*   - 本层不得 #include "qpc.h"（见 07-接口约定#总原则 3）
*   - 不做 I/O 等待：只负责格式化与入队，真正的发送在中断里完成
*   - **禁止在中断上下文调用**（格式化会占用较多栈与时间）
*===========================================================================*/
#ifndef SVC_LOG_H
#define SVC_LOG_H

#include "app_config.h"

/* 日志等级（与 app_config.h 的配置值对应） */
typedef enum {
    SVC_LOG_NONE = APP_CFG_LOG_LVL_NONE,
    SVC_LOG_ERROR = APP_CFG_LOG_LVL_ERROR,
    SVC_LOG_WARN = APP_CFG_LOG_LVL_WARN,
    SVC_LOG_INFO = APP_CFG_LOG_LVL_INFO,
    SVC_LOG_DEBUG = APP_CFG_LOG_LVL_DEBUG
} svc_log_lvl_t;

/*! 格式化并异步输出一条日志（带等级前缀与 CRLF）。
* @param lvl 等级；大于 APP_CFG_LOG_LEVEL 时本函数内部直接返回
* @note 运行期等级判断保留在函数内部，调用点仍然不需要 if。
*/
void SVC_Log_Printf(svc_log_lvl_t lvl, char const *fmt, ...);

/*! 原样异步输出一段文本（不加等级前缀、不加 CRLF）。
* 用于 AT 响应这类需要精确控制字节流的内容。
*/
void SVC_Log_Write(char const *str);

/* 编译期开关：关闭时全部展开为空语句 ---------------------------------------*/

#if (APP_CFG_LOG_ENABLE != 0)

    #if (APP_CFG_LOG_LEVEL >= APP_CFG_LOG_LVL_ERROR)
        #define LOG_E(...)  SVC_Log_Printf(SVC_LOG_ERROR, __VA_ARGS__)
    #else
        #define LOG_E(...)  ((void)0)
    #endif

    #if (APP_CFG_LOG_LEVEL >= APP_CFG_LOG_LVL_WARN)
        #define LOG_W(...)  SVC_Log_Printf(SVC_LOG_WARN, __VA_ARGS__)
    #else
        #define LOG_W(...)  ((void)0)
    #endif

    #if (APP_CFG_LOG_LEVEL >= APP_CFG_LOG_LVL_INFO)
        #define LOG_I(...)  SVC_Log_Printf(SVC_LOG_INFO, __VA_ARGS__)
    #else
        #define LOG_I(...)  ((void)0)
    #endif

    #if (APP_CFG_LOG_LEVEL >= APP_CFG_LOG_LVL_DEBUG)
        #define LOG_D(...)  SVC_Log_Printf(SVC_LOG_DEBUG, __VA_ARGS__)
    #else
        #define LOG_D(...)  ((void)0)
    #endif

    #define LOG_WRITE(str_)  SVC_Log_Write(str_)

#else /* 日志总开关关闭 */

    #define LOG_E(...)       ((void)0)
    #define LOG_W(...)       ((void)0)
    #define LOG_I(...)       ((void)0)
    #define LOG_D(...)       ((void)0)
    #define LOG_WRITE(str_)  ((void)0)

#endif /* APP_CFG_LOG_ENABLE */

#endif /* SVC_LOG_H */
