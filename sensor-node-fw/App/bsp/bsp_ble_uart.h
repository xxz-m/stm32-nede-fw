/*============================================================================
* bsp_ble_uart.h —— BLE 模组串口平台端口（实例由装配点注入）
* 层次定位：BSP；唯一接触 HAL 的 BLE 串口实现。
*
* NOTE (实例不写死)：本端口不再固定占用某一路 USART —— 板子改版只改
*   app_config.h 的 APP_CFG_BLE_UART_ID，由 AO_Ble 在构造时传给
*   BSP_bleUartInit()。当前为 USART2（PA2/PA3，见 docs_sensor/03-资源分配.md）。
*===========================================================================*/
#ifndef BSP_BLE_UART_H
#define BSP_BLE_UART_H

#include "bsp_uart.h"   /* bsp_uart_id_t：BSP 自己的实例编号，不含 HAL 类型 */
#include "ble_port.h"
#include <stdbool.h>
#include <stdint.h>

/** @brief 接收通知回调；运行在 ISR 上下文，只允许投递事件 */
typedef void (*BspBleRxSink)(void);

/** @brief 初始化 BLE 串口链路并把 Port 回调填进调用方的函数表。
* @param port [out] 由调用方（AO 的组件实例）持久的端口对象；不得为 NULL
* @param uart 本端口占用的 BSP 串口实例编号（取值见 app_config.h 的 APP_CFG_BLE_UART_ID）
* @note  含模组电源使能（VG6_EN/PB1 拉高）与首次接收装载，返回后链路即待命
* @note  编号非法（BSP_UART_NONE / BSP_UART_1）时直接返回，不配置任何外设 */
void BSP_bleUartInit(BlePort *port, bsp_uart_id_t uart);

/** @brief 本端口是否占用该实例。
* @note  由 bsp_uart.c 的 HAL 公共回调按 huart->Instance 分派调用，中断上下文 */
bool BSP_bleUartIsInstance(bsp_uart_id_t uart);

/** @brief 注册接收通知回调；NULL 表示注销 */
void BSP_bleUartSetSink(BspBleRxSink sink);

/* 中断服务侧入口（由 bsp_uart.c 的 HAL 公共回调按实例分发） */
void BSP_bleUartRxComplete(void);
void BSP_bleUartTxComplete(void);
void BSP_bleUartError(void);

/* 诊断计数 */
uint32_t BSP_bleUartOverflow(void);
uint32_t BSP_bleUartErrorCount(void);
uint32_t BSP_bleUartRxCount(void);
uint32_t BSP_bleUartTxCount(void);

/** @brief 本端口**实际生效**的波特率（读自外设句柄，不是配置宏）。
* @note  启动日志用它，避免「宏改了但没生效」被日志掩盖；理由见
*        bsp_modem_uart.c 中「波特率必须真正写进外设」的 NOTE。 */
uint32_t BSP_bleUartBaud(void);

#endif
