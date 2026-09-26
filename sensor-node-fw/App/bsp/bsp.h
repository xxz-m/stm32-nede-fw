/*============================================================================
* bsp.h —— 板级支持层（BSP）聚合头
*
* 层次定位：App → Service → Driver → **BSP** → HAL/CMSIS
*           BSP 是唯一允许接触 HAL 与寄存器的自研层。
*
* 本层按职责拆成多个文件，本头文件是统一对外入口：
*   bsp.h           本文件：公共类型、QP 节拍、时间基准、BSP_init()
*   bsp_uart.h/.c   USART1：中断收发、接收环形缓冲、异步日志发送队列
*   bsp_i2c.h/.c    I2C1：非阻塞中断传输、完成回调、总线恢复（参数存储用）
*   bsp.c           QP 要求的钩子（QF_onStartup / QV_onIdle / 断言钩子等）
*
* 约束（见 docs_sensor/07-接口约定.md#BSP 层）：
*   - 本层头文件不得包含任何上层头文件（App/Service/Driver），也不得暴露 HAL 类型
*   - 不需要认识活动对象：中断到应用的接线用 BSP_uartRxSetSink() 注册回调完成
*   - 每个 ISR 安全的函数必须在注释里写明「由谁调用」
*
* 目标硬件：STM32F103RCT6（CubeMX 生成的基础工程）
* 框架    ：QP/C 6.9.1，QV 协作式内核
* 时基    ：HAL 时基在 TIM1，SysTick 专供 QP 节拍（100 Hz）
*
* 本目录不受 STM32CubeMX 管理：放在 App/ 下，重新生成代码不会影响它。
*===========================================================================*/
#ifndef BSP_H
#define BSP_H

#include <stdint.h>
#include <stdbool.h>

/* 全局配置（所有层都可依赖，见 AGENTS.md 第 5.1 节例外条款）*/
#include "app_config.h"

/* 每秒的 QP 节拍数（SysTick 周期 = 1/BSP_TICKS_PER_SEC） */
#define BSP_TICKS_PER_SEC   (100U)

/* BSP 层状态码（本层专用，不跨层复用） */
typedef enum {
    BSP_OK = 0,      /* 成功               */
    BSP_ERR_PARAM,   /* 入参非法           */
    BSP_ERR_BUSY,    /* 外设忙             */
    BSP_ERR_TIMEOUT, /* 操作超时           */
    BSP_ERR_IO       /* 总线 / 外设错误    */
} bsp_status_t;

/* 串口与日志子模块（对外仍只需 #include "bsp.h"） -------------------------*/
#include "bsp_uart.h"
#include "bsp_ble_uart.h"
#include "bsp_modem_uart.h"
/* I2C 子模块。NOTE (顺序)：必须放在 bsp_status_t 定义之后（bsp_i2c.h 会用到它）。 */
#include "bsp_i2c.h"

/* 初始化 -------------------------------------------------------------------*/

/*! 初始化板级支持：把 SysTick 配成 QP 时钟节拍、装载串口中断接收、打印横幅。
* @note 必须在 QF_init() 以及 HAL/外设初始化之后调用。
* @note 本函数不认识任何活动对象；AO 的装配在 App 层的 App_start()。
*/
void BSP_init(void);

/* 时间基准 -----------------------------------------------------------------*/

/*! 上电以来的毫秒数（来源 HAL 时基 TIM1）。 */
uint32_t BSP_uptimeMs(void);

/*! 毫秒级延时。
* @warning 只允许在初始化阶段使用；AO 状态处理函数里禁止调用（阻塞预算见
*          07-接口约定#时序与阻塞预算）。
*/
void BSP_delayMs(uint32_t ms);

/*! 请求 MCU 系统复位；仅由已认证的应用命令调用。 */
void BSP_systemReset(void);

#endif /* BSP_H */
