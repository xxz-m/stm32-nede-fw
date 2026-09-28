/*============================================================================
* system.h —— AO_System：系统级动作（延迟复位）的唯一管理者
*
* 层次定位：App 层。只依赖 BSP（复位、串口排空）与 App 层的两个**只读**查询
*           （`App_Store_IsIdle()` / `Modem_IsUplinkIdle()`），不认识任何具体命令。
* 目标硬件：STM32F103RCT6（LQFP64）
* 所属内核：QP/C 6.9.1 · QV（协作式、非抢占、单栈）
*
* 为什么要有这个 AO（2026-09-28 新增，配合 `AT+REBOOT`）：
*   「重启设备」这条命令**绝不能在命令执行路径里就地复位**。复位之前必须保证：
*     ① 命令的回复已经出了设备——4G 要等 `AT+MQTTPUB` 走完（模组回 `OK`），
*        串口要等异步发送环排空。否则平台判「指令超时」、串口上什么都看不到，
*        而设备已经重启，两边现象对不上，是最难查的一类现场；
*     ② 刚改过的参数已经落盘——EEPROM 写入由 AO_Store 异步完成（SAVE → VERIFY），
*        复位打断它，「改完紧接着重启」就会把值丢掉；
*     ③ 上面两条要有**最短延时**（保证回复有机会出去）与**硬上限**（保证一定能重启，
*        不会被"等条件"卡成永不重启）。
*   这些判据集中在一个 AO 里，是为了做到「谁都可以请求重启，但只有一处真正复位」。
*
* 接口约定：任何来源（串口、4G、将来的 BLE）都只调 `App_System_RequestReboot()`，
*   它只投一个事件、不改任何状态；倒计时与复位全部发生在 AO_System 的状态处理函数里。
*===========================================================================*/
#ifndef APP_SYSTEM_H
#define APP_SYSTEM_H

#include "qpc.h"

/*! 谁请求了这次重启。只用于复位前那一行日志（现场排障要能区分来源）。 */
typedef enum {
    APP_REBOOT_SOURCE_SERIAL = 1,  /*!< 调试串口 `AT+RST` / `AT+REBOOT`  */
    APP_REBOOT_SOURCE_CLOUD,       /*!< 平台 command 主题下发            */
    APP_REBOOT_SOURCE_BLE,         /*!< 预留：BLE 侧将来统一走这里       */
    APP_REBOOT_SOURCE_IDENTITY     /*!< **身份参数被改动**后的自动重启
                                    *   （当前：串口写 `AT+SN=` 且值真的变了）*/
} AppRebootSource;

/* AO 生命周期（装配点在 App/app/app.c 的 App_start()） ----------------------*/

/*! 构造：初始化时间事件与"未请求"状态。 */
void System_ctor(void);

/*! 启动 AO（事件队列存储由本模块自己持有）。 */
void System_start(void);

/*! 单例句柄。 */
extern QActive * const AO_System;

/* 给命令分发器用的请求入口 ------------------------------------------------*/

/**
 * @brief 请求一次系统复位（异步、幂等、可重复调用）。
 *
 * @param source 请求来源（只用于日志）
 *
 * NOTE (为什么只投事件、不直接改状态)：本函数会被 AO_Node / AO_Modem 的处理函数
 *   在各自的上下文里调用。把状态留给 AO_System 自己维护，就不会出现「两个 AO
 *   同时写同一份倒计时」的共享可变状态（见 AGENTS.md 5.2）。
 * NOTE (幂等)：已经在倒计时里再调一次不会重置计时——平台重发、串口连敲都不会
 *   把重启越推越晚。日志在 AO_System 收到事件时才打，也能自然去重。
 * NOTE (不阻塞)：本函数立即返回，因此可以安全地在命令分发器里调用。
 */
void App_System_RequestReboot(AppRebootSource source);

#endif /* APP_SYSTEM_H */
