/*============================================================================
* store.h —— 应用层：AO_Store（掉电参数存储 + 调试口参数读写）
*
* 层次定位：**App(应用层)** → Service → Driver → BSP → HAL/CMSIS
*           本文件只暴露「应用怎么用存储」，不暴露任何 HAL / I2C 细节。
*
* 职责：
*   1. 持有参数集合的 **RAM 影子**（`SvcParam`），它是运行期的唯一真相
*   2. 上电异步把介质里的有效记录读进影子（A/B 双槽取最新、校验 CRC）；
*      介质空白或损坏时回落到 app_config.h 的编译期默认值
*   3. 把运行期的修改异步落盘（交替写另一个槽 + 回读校验），全程不阻塞调用者
*   4. 向其它 AO 提供只读访问（`App_Store_Value()`）与「配置是否已就绪」
*      （`App_Store_ConfigResolved()`，供 AO_Ble 在探测模组前等一次）
*
* 为什么单独一个 AO（而不是塞进 AO_Node / AO_Ble）：
*   - 存储与「谁在用参数」无关：BLE、4G、将来的产测都要它，不能绑在某个通信 AO 上，
*     也不能随 APP_CFG_BLE_ENABLE 一起消失；
*   - 写一次记录要跨十几拍总线（页拆分 + 写周期等待），需要一个专门的心跳推进；
*   - 装配步骤见 docs_sensor/05-任务说明.md#新增 AO 的步骤。
*
* 并发与数据流（都在 QV 同一个主栈上，不抢占）：
*   写：AT 命令 → App_Store_SetName()（立刻改影子并置 dirty）→ 投 STORE_CMD_SIG
*        → AO_Store 在 ST_READY 看到 dirty → 编码 → 交替槽写入 → 回读校验 → 清 dirty
*   读：其它 AO / 命令处理直接读影子（App_Store_Value），不碰总线
*   完成通知：I2C 中断 → Store_i2cSink() → 投 STORE_BUS_SIG（只投递，不做业务）
*===========================================================================*/
#ifndef APP_STORE_H
#define APP_STORE_H

#include "qpc.h"
#include "svc_param.h"

#include <stdbool.h>
#include <stdint.h>

/*! 存储运行状态快照（给调试口 `AT+CFG?` 用，都是只读量）。 */
typedef struct {
    bool     present;         /*!< 介质是否应答（EEPROM 在不在）      */
    bool     resolved;        /*!< 启动加载阶段是否已结束（成功或失败）*/
    bool     loaded;          /*!< 影子是否来自介质上的有效记录        */
    bool     dirty;           /*!< 有修改还没落盘                      */
    uint8_t  slot_seq;        /*!< 最近一次保存用的槽序号              */
    uint16_t config_version;  /*!< 配置版本号（每次成功保存自增）      */
    uint32_t saved;           /*!< 成功保存次数（诊断）                */
    uint32_t failed;          /*!< 失败的读写次数（诊断）              */
} AppStoreInfo;

/* AO 生命周期（装配点在 App/app/app.c 的 App_start()） ----------------------*/

/*! 构造：清零状态 + 把影子填成编译期默认值。
* @note 必须在任何 `App_Store_Value()` 调用之前执行——`Ble_ctor()` 会用它填配置，
*       因此装配点里本函数要排在其它 AO 的 _ctor() 之前。
*/
void Store_ctor(void);

/*! 启动 AO（事件队列存储由本模块自己持有）。 */
void Store_start(void);

/*! I2C 中断的完成端（由 App 装配点用 BSP_i2cSetSink() 注册）。
* @note 在中断上下文运行：只投递事件，不做任何业务。
*/
void Store_i2cSink(void);

/*! 单例句柄。 */
extern QActive * const AO_Store;

/* 给其它 AO / 命令处理用的只读与写入口 -------------------------------------*/

/*! 取参数当前值（永远返回合法 C 字符串，不会返回 NULL）。
* @note 返回的是影子内部的地址，**只要进程活着就一直有效**；调用方不需要拷贝。
*       因此 `BleConfig` 可以直接持有这些指针：介质读完后影子被就地覆盖，
*       之后读到的就是新值（BLE 下次配置模组时自然生效）。
*/
char const *App_Store_Value(uint8_t id);

/*! 启动加载阶段是否已结束。AO_Ble 用它把「探测模组」推迟到配置就绪之后；
* 存储功能关闭时立刻为 true，不会把 BLE 卡住。 */
bool App_Store_ConfigResolved(void);

/*! 按 AT 名设置一个参数（只改影子，落盘由 AO 异步完成）。
* @param name  AT 名（大小写不敏感，见 svc_param.h 的参数表）
* @param value 新值（非空）
* @return SVC_OK / SVC_ERR_PARAM（参数名不认识）/ SVC_ERR_RANGE（值超长或为空）
*         / SVC_ERR_STATE（启动加载还没结束，此时改会被随后的读取覆盖）
*/
svc_status_t App_Store_SetName(char const *name, char const *value);

/*! 把所有参数恢复成编译期默认值并落盘（调试口 `AT&F`）。
* @return true = 已受理（异步落盘）；false = 启动加载还没结束，本次未改动
* @note 保留 `config_version` 并让它继续自增：恢复默认值也是一次变更，
*       把它倒回 0 会让云端「版本必须递增」的判断失效。
*/
bool App_Store_ResetDefaults(void);

/*! 请求一次 I2C 地址扫描（调试口 `AT+I2C?`）。
* @return true = 已受理（结果以 `+I2C:<地址>` URC 打到串口，末尾 `+I2C:END`）；
*         false = 存储关闭 / 未就绪 / 已有扫描在跑
*/
bool App_Store_RequestScan(void);

/*! 取运行状态快照。 */
void App_Store_Info(AppStoreInfo *out);

#endif /* APP_STORE_H */
