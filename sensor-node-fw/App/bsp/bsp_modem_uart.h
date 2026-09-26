/*============================================================================
* bsp_modem_uart.h —— 4G 模组 AT 串口平台端口（实例由装配点注入）
*
* 层次定位：BSP 板级支持层（唯一允许接触 HAL 与寄存器的一层，见 AGENTS.md 5.1）
* 目标硬件：STM32F103RCT6；模组 AT 口占哪一路由 APP_CFG_MODEM_UART_ID 决定，
*           当前为 USART3 = PB10(TX) / PB11(RX)（2026-09-20 与 BLE 对调）
* 所属内核：QV（协作式）——接收中断只做「存字节 + 通知 sink + 重装外设」
* 生效条件：仅当 app_config.h 的 APP_CFG_MODEM_ENABLE != 0 时编译
*
* NOTE (实例不写死)：端口不再硬编码 huartN；改接线只改 app_config.h 的
*   APP_CFG_MODEM_UART_ID，由 AO_Modem 在构造时传给 BSP_modemUartInit()。
* NOTE (缓冲纪律)：本模块的环形缓冲一律 static，禁止放到栈上——QV 单栈只有
*   4 KB（startup_stm32f103xe.s 的 Stack_Size = 0x1000），爆栈会直接跑飞。
* NOTE (中断优先级)：承载模组 AT 的那一路 USART 抢占优先级必须 >= 3（QF-aware），
*   由 CubeMX 的 .ioc 配置为 5；此处不改优先级，避免覆盖 .ioc 这一唯一真相。
*===========================================================================*/
#ifndef BSP_MODEM_UART_H
#define BSP_MODEM_UART_H

#include "app_config.h"
#include "bsp_uart.h"   /* bsp_uart_id_t：BSP 自己的实例编号，不含 HAL 类型 */
#include "modem_port.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if (APP_CFG_MODEM_ENABLE != 0U)

/** @brief 接收通知回调；运行在 ISR 上下文，只允许投递事件，禁止解析、日志与阻塞 */
typedef void (*BspModemRxSink)(void);

/** @brief 初始化模组串口收发链路并把四个 Port 回调填进调用方的函数表。
* @param port [out] 由调用方（AO 的组件实例）持久的端口对象；不得为 NULL
* @param uart 本端口占用的 BSP 串口实例编号（取值见 app_config.h 的 APP_CFG_MODEM_UART_ID）
* @note  含首次接收装载；调用后接收链路即处于待命状态
* @note  编号非法（BSP_UART_NONE / BSP_UART_1）时直接返回，不配置任何外设 */
void BSP_modemUartInit(ModemPort *port, bsp_uart_id_t uart);

/** @brief 本端口是否占用该实例。
* @note  由 bsp_uart.c 的 HAL 公共回调按 huart->Instance 分派调用，中断上下文 */
bool BSP_modemUartIsInstance(bsp_uart_id_t uart);

/** @brief 模组电源开关（`MN316_EN`，高开启 / 低关闭）。
* @param on true = 上电，false = 断电
* @note  由 AO_Modem 用作最后手段：连续几轮连接失败后把模组彻底掉电复位。
* @note  **非阻塞**：只写引脚电平、不等待；断电时长由调用方（AO 的周期事件）计时。
* @note  重新上电后应再调一次 `BSP_modemUartInit()`：它会清空收发环形缓冲并重装
*        单字节接收，避免断电期间的残字节被当成应答。
* @note  低电平会切断模组供电，调用前应确认链路确实不可用。 */
void BSP_modemPowerSet(bool on);

/** @brief 注册接收通知回调
* @param sink 回调函数；传 NULL 取消注册
* @note  回调运行在 ISR 上下文 */
void BSP_modemUartSetSink(BspModemRxSink sink);

/** @brief 接收溢出累计计数（诊断用，只增不减） */
uint32_t BSP_modemOverflowCount(void);

/** @brief 串口错误累计计数（ORE/FE/NE/PE，诊断用） */
uint32_t BSP_modemErrorCount(void);

/** @brief 累计收到的字节数（诊断用，只增不减）。
* @note  与 uart_err 配合判读：「rx=0 且 uart_err=0」= 线上一片安静（没供电、
*        被摁在复位、线没接、或者对方根本不应答）；「rx>0 且 uart_err>0」= 字节来了
*        但成帧失败，多半是波特率不对。 */
uint32_t BSP_modemUartRxCount(void);

/** @brief 本端口**实际生效**的波特率（读自外设句柄，不是配置宏）。
* @note  启动日志用它，避免「宏改了但没生效」被日志掩盖（见 bsp_modem_uart.c 的 NOTE）。 */
uint32_t BSP_modemUartBaud(void);

/* 中断服务侧入口（由 bsp_uart.c 中的 HAL 公共回调按 huart->Instance 分发，
 * 不在本文件定义 HAL_UART_*Callback，避免与之重复定义） */
void BSP_modemUartRxComplete(void);
void BSP_modemUartTxComplete(void);
void BSP_modemUartError(void);

#endif /* APP_CFG_MODEM_ENABLE != 0 */
#endif /* BSP_MODEM_UART_H */
