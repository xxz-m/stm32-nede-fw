/*============================================================================
 * store.h —— App 层：AO_Store 参数影子与 NOR 配置日志编排
 * 目标：STM32F103RCT6；内核：QP/C 6.9.1 QV；通过 Service 与抽象 Driver 解耦芯片。
 * 上电扫描 CRC 与提交标记后选择最新快照；正文回读、提交、最终回读后才清 dirty。
 * 保存期间的后续编辑保留 dirty；失败保留影子，有限重试后等待下一次编辑。
 * SPI DMA 由周期推进；I2C sink 仅保留兼容诊断，不用于 Flash 完成通知。
 *===========================================================================*/
#ifndef APP_STORE_H
#define APP_STORE_H

#include "qpc.h"
#include "svc_param.h"

#include <stdbool.h>
#include <stdint.h>

/*! 存储运行状态快照（给调试口 `AT+CFG?` 用，都是只读量）。 */
typedef struct {
    bool     present;         /*!< 介质是否应答（Flash 探测与扫描是否成功）      */
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

/*! 存储是否"已经没事干了"：可以安全复位。
*
* @return true = 启动加载已结束，且**没有未落盘的改动**，也没有正在进行的读写。
*
* NOTE (谁在用)：`AO_System` 在重启倒计时里等这个条件——复位会打断 Flash 编程，
*   「`AT+TRANSINTVL=5` 紧接着 `AT+REBOOT`」必须等前者落完盘才允许复位。
* NOTE (为什么不是只看 dirty)：dirty 是在提交标记与最终回读通过之后才清的，所以
*   `!dirty` 已经蕴含「不在写」；这里再看一眼相位，是为了把"还没开始加载"的
*   上电窗口也算成忙（那时改参数本来就会被拒），避免刚上电就被复位。
* NOTE (存储关闭时)：`APP_CFG_STORE_ENABLE=0` 时永远返回 true（没有可丢的东西）。
*/
bool App_Store_IsIdle(void);

#endif /* APP_STORE_H */
