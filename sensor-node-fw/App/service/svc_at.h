/*============================================================================
* svc_at.h —— 服务层：AT 指令解析（调试串口）
*
* 层次定位：App → **Service(服务层)** → BSP → HAL/CMSIS
*
* 本模块只做**解析**：把一行文本翻成一个 `SvcAtCmd`，不产生任何响应文本，
* 也不碰总线与存储。命令的**执行与回复**由调用方（App 层的 AO_Node）负责——
* 因为参数读写要落到 AO_Store 上异步完成，Service 层不允许反向调用 App。
*
* 支持的命令（按 V.250 / 3GPP 27.007 的常规形式；大小写不敏感）：
*
*   AT                     握手                   -> OK
*   AT&F                   恢复编译期默认值并落盘   -> OK
*   AT+CFG?                查参数存储运行状态       -> +CFG:<8 个逗号分隔字段>
*   AT+I2C?                扫描 I2C 总线            -> OK（结果以 +I2C:<地址> 上报）
*   AT+VER?                查固件版本与编译时间     -> +VER:"<版本>","<日期>"
*   AT+CMD?                查本设备支持的 AT 命令集  -> +CAD:…
*   AT+REBOOT / AT+RST     重启设备（先回 OK，延时后复位）-> OK
*   AT+BLESVCUUID?         查 BLE 服务 UUID（= 参数 `uuid`）-> +BLESVCUUID:"<值>"，随后 OK
*   AT+<参数名>?           读参数                   -> +<参数名>:"<值>"（数值型不带引号）
*   AT+<参数名>="<值>"     写参数                   -> OK
*   AT+<参数名>=?          测试形式                 -> +<参数名>:(<最大长度>)
*
* 参数名（8 个）：SN NAME PIN UUID DEVNAME MODEL BATCH TRANSINTVL（大小写不敏感）。
*   - 前 7 个是**字符串型**：读回带引号、测试形式报可写字符数；
*   - `TRANSINTVL` 是**数值型**（周期上报间隔，分钟，1~20）：读回不带引号
*     （`+TRANSINTVL:20`）、测试形式报范围（`+TRANSINTVL:(1,20)`）、写越界回
*     `+CME ERROR:2`。形态差异的实现在 `command_dispatch.c`（本层只解析，不判范围）。
*
* NOTE (2026-09-27 接口改写)：本模块原先支持 `sn?` / `sn=值` / `cfg?` / `cfg!` / `i2c?`
*   这套**自定义语法**——借用了 AT 的 `?` `=` `ok` `ERROR` 外壳，但不是 AT 语法：
*   缺 `AT+` 前缀、`!` 不是 AT 的语法元素、错误码用 `+err:<n>`、`ok` 与 `ERROR` 大小写都不统一。
*   本次按标准 AT 重写，**且不再兼容旧写法**。
*
* NOTE (同时删掉的一条捷径)：原先为了「裸 `at` 不带行尾也回 ok」，解析器在缓冲区恰好等于
*   `at` 时就立即触发。改成 `AT+…` 语法后，输入 `AT+SN?` 的前两个字符就是 `AT`，
*   **会在第 2 个字符处误触发并清空缓冲区**（后面那截 `+SN?` 会变成一条独立的非法行）。
*   因此该捷径已删除：所有命令（含 `AT` 自身）都必须以行尾结束。
*
* 约定：
*   - 无状态优先：解析状态由调用方（AO）持有，作为第一个参数传入
*   - 不做 I/O：只解析，不发送、不读存储
*   - 本层不得 #include "qpc.h"，禁止在中断上下文调用
*   - `APP_CFG_AT_ENABLE = 0` 时本模块整体变成空操作（调用点不用改）
*===========================================================================*/
#ifndef SVC_AT_H
#define SVC_AT_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "app_config.h"

/* 命令行的最大长度（含结尾 '\0' 的位置）。
* `AT+DEVNAME="<24 字符>"` 是 35 字符，`AT+UUID="<36 字符>"` 是 47 —— 都远在 64 以内。 */
#define SVC_AT_LINE_MAX  (APP_CFG_AT_LINE_MAX)

/*! 参数名的最大长度（含结尾 '\0'）。当前最长的是 "DEVNAME"（7 字符）。 */
#define SVC_AT_NAME_MAX  (16U)

/*! 参数值的最大长度；值与命令同名空间，故取整行长度即可。 */
#define SVC_AT_VALUE_MAX (SVC_AT_LINE_MAX)

/*! 调用方为一条响应准备的缓冲大小。
* 最长的响应是 `+CFG:` 那行（8 个十进制字段）与 `+DEVNAME:"…"`，约 50 字节，留足余量。 */
#define SVC_AT_RESP_MAX  (SVC_AT_LINE_MAX + 128U)

/*! 解析出的命令种类。 */
typedef enum {
    SVC_AT_KIND_NONE = 0,   /*!< 没解析出命令（未识别 / 空行） */
    SVC_AT_KIND_PING,       /*!< AT                            */
    SVC_AT_KIND_REGISTER_READ,  /*!< AT+READ="地址列表,CRC" */
    SVC_AT_KIND_REGISTER_WRITE, /*!< AT+WRITE="地址,值,...,CRC" */
    SVC_AT_KIND_GET,        /*!< AT+<参数名>?                  */
    SVC_AT_KIND_SET,        /*!< AT+<参数名>="<值>"            */
    SVC_AT_KIND_TEST,       /*!< AT+<参数名>=?                 */
    SVC_AT_KIND_INFO,       /*!< AT+CFG?                       */
    SVC_AT_KIND_DEFAULTS,   /*!< AT&F                          */
    SVC_AT_KIND_SCAN,        /*!< AT+I2C?                       */
		SVG_AT_KIND_CMD,				/*!< AT+VER?                       */
		SVC_AT_KIND_VERSION,			 /*!< AT+VER?                       */
		SVC_AT_KIND_REBOOT,			 /*!< AT+REBOOT（系统复位）          */
		SVC_AT_KIND_BLE_UUID			 /*!< AT+BLESVCUUID?（查 BLE 服务 UUID）*/
		
} SvcAtKind;

/*! 一条解析结果。 */
typedef struct {
    SvcAtKind kind;
    char      param[SVC_AT_NAME_MAX];   /*!< GET / SET / TEST 的参数名（已转小写） */
    char      value[SVC_AT_VALUE_MAX];  /*!< SET 的值（已去掉外层引号）           */
} SvcAtCmd;

/*! AT 解析器状态：由调用方（App 层的 AO）持有。 */
typedef struct {
    char    line[SVC_AT_LINE_MAX];
    uint8_t len;   /* 已累积的字符数（不含结尾 '\0'） */
} SvcAt;

/*! 初始化解析器（清空缓冲区）。 */
void SVC_At_Init(SvcAt *me);

/*! 喂入一个接收字节。
* @param me   解析器状态
* @param byte 本次收到的字节
* @param out  出参：解析到完整命令时填入（此时返回 true）
* @return true 表示解析出一条命令（`out->kind` 一定不是 NONE）
* @note 收到 CR 或 LF 视为一行结束；**命令必须带行尾**（见文件头关于裸 `at` 的 NOTE）。
*       行内无法识别时不产生命令（返回 false），由调用方按 AT 惯例回 `ERROR`。
*/
bool SVC_At_Feed(SvcAt *me, uint8_t byte, SvcAtCmd *out);

#endif /* SVC_AT_H */
