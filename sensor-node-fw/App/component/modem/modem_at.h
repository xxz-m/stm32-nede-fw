/*============================================================================
* modem_at.h —— 芯片无关的增量式 AT 引擎
* 层次定位：Component 层内部头；只处理 AT 协议的公共部分，不认识任何业务指令。
* 目标硬件：与具体模组无关（MN316 / NT26 共用）
*
* 设计要点（为什么这样切）：
*   - 引擎不碰串口，也不阻塞。调用方按「收完一行就喂一行」的方式驱动它，
*     这样 QV 的单栈上永远不会出现「等模组应答」的循环。
*   - 期望应答标记按行匹配：AT 应答的一行就是一条语义完整的应答，
*     行内匹配既够用，又不会像「跨行 strstr」那样把上一条命令的残留误判成
*     下一条命令的应答。
*   - 超时不做忙等，只比较期限（now - deadline）；调用方每个 Process 周期
*     调一次 ModemAt_Tick() 即可。
*===========================================================================*/
#ifndef MODEM_AT_H
#define MODEM_AT_H

#include "modem_component_config.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** AT 引擎阶段。 */
typedef enum {
    MODEM_AT_IDLE = 0,  /* 空闲：没有命令在途                                        */
    MODEM_AT_WAITING    /* 等待应答：命令已写出，调用方正在喂行与查超时              */
} ModemAtPhase;

/** AT 引擎结果。PENDING 表示还没结论，其余四个都是终态。 */
typedef enum {
    MODEM_AT_PENDING = 0,  /* 命令在途，继续喂行 / 继续等                       */
    MODEM_AT_DONE,         /* 收到期望应答标记                                  */
    MODEM_AT_ERROR,        /* 收到 ERROR / +CME ERROR / +CMS ERROR              */
    MODEM_AT_TIMEOUT       /* 超过期限仍未收到期望应答                          */
} ModemAtResult;

/** 一条命令的执行上下文；由组件持有（静态分配，无动态内存）。 */
typedef struct {
    ModemAtPhase phase;
    ModemAtResult result;
    char expected[MODEM_COMPONENT_TOKEN_MAX];  /* 期望应答标记，如 "OK" / "+MQTTSUBACK" */
    uint32_t deadline_ms;                      /* 应答期限，32 位自然回绕 */
} ModemAt;

/** @brief 复位引擎到空闲态；组件 Init 时调用一次。 */
void ModemAt_Init(ModemAt *me);

/** @brief 丢弃当前命令回到空闲态（用于重连或放弃一条命令）。 */
void ModemAt_Reset(ModemAt *me);

/** @brief 记下一次「命令已写出」，进入等待应答状态。
* @param expected 期望应答标记；为空串时按「任意 OK 行」处理由调用方保证
* @param now_ms 当前毫秒时基
* @param timeout_ms 应答期限（毫秒）；0 表示用组件默认超时 */
void ModemAt_Begin(ModemAt *me, char const *expected, uint32_t now_ms, uint32_t timeout_ms);

/** @brief 喂入一整行（不含行尾 CR/LF，以 NUL 结尾）。
* @return 引擎的最新结果；调用方据此决定下一步 */
ModemAtResult ModemAt_Feed(ModemAt *me, char const *line, size_t length, uint32_t now_ms);

/** @brief 只推进超时判定，不喂数据。
* @return 命令在途且已超期返回 MODEM_AT_TIMEOUT，否则返回当前结果 */
ModemAtResult ModemAt_Tick(ModemAt *me, uint32_t now_ms);

/** @brief 判断一行文本是否为 AT 通用错误行（ERROR / +CME ERROR / +CMS ERROR）。 */
bool ModemAt_LineHasError(char const *line);

/** @brief 判断一行文本是否含指定应答标记。 */
bool ModemAt_LineHasToken(char const *line, char const *token);

/** @brief 从游标处取下一个十进制整数，并把游标推到该整数之后。
* @param cursor [in,out] 文本游标；调用后指向整数末尾
* @param out [out] 取到的整数值
* @return 取到整数返回 true；游标为空或找不到数字返回 false
* @note 只认 0-9，不支持 '+' 号与科学计数法——AT 应答里的小区状态、CSQ、
*       通道号都是这种形态，省掉 strtol 可以把它的体积留在库外面。 */
bool ModemAt_NextInt(char const **cursor, int32_t *out);

/** @brief 从游标处取下一个双引号字段，并把游标推到右引号之后。
* @param cursor [in,out] 文本游标
* @param out [out] 字段内容（不含引号）
* @param capacity [in] 输出缓冲总容量（含终止符）
* @return 取到非空字段且放得下返回 true */
bool ModemAt_NextQuoted(char const **cursor, char *out, size_t capacity);

/** @brief 从游标处取下一个字段，**自动适配带引号与不带引号两种形态**。
* @param cursor [in,out] 文本游标
* @param out [out] 字段内容（不含引号、去掉首尾空格）
* @param capacity [in] 输出缓冲总容量（含终止符）
* @return 取到非空字段且放得下返回 true
* @note 为什么要这个：同一份「位置信息」在不同模组上的形态并不一致——
*   ML307C 的 AT+CGI 回的是 `"250ba81","146c"`（带引号），
*   MN316 的 +CEREG 回的是 `+CEREG:2,1,6E55,02516E85,9`（**不带引号**）。
*   用 NextQuoted() 去读后者会直接失败（找不到引号），字段就永远取不到。
* @note 宽容度：读取前会跳过前导的空格/制表符**与逗号**（NextInt() 读完数字后游标停在
*   分隔符上，不跳逗号会读到空字段）；裸文本字段的尾部空格也会去掉。
*   NOTE (不要「先试 NextQuoted 再退回 token」)：NextQuoted() 内部是 strchr('"')，
*   对不带引号的输入会去找**后面**的引号，可能把别的字段读进来。所以必须先看
*   光标处第一个非空白字符是不是引号，再决定用哪种读法。 */
bool ModemAt_NextField(char const **cursor, char *out, size_t capacity);

/** @brief 把 "104.071103" 这类定点文本按 1e-6 度解析成整数。
* @param out [out] 结果，单位 1e-6 度
* @return 解析成功返回 true
* @note 用整数而不是 float：Cortex-M3 没有硬件浮点，经纬度只需 1e-6 度精度。 */
bool ModemAt_ParseE6(char const *text, int32_t *out);

/** @brief 检查文本能否安全放进 AT 的引号参数（不含双引号与换行符）。 */
bool ModemAt_IsSafeText(char const *text);

/** @brief 把文本编码成十六进制字符串（AT 引号参数的十六进制形式）。
* @param out 输出缓冲；capacity 为总容量（含终止符）
* @return 放得下返回 true；放不下返回 false，调用方不得截断发送 */
bool ModemAt_HexEncode(char *out, size_t capacity, char const *text);

/** @brief 期限是否已到（带符号比较，兼容 32 位回绕）。 */
bool ModemAt_DeadlineReached(uint32_t now_ms, uint32_t deadline_ms);

#endif /* MODEM_AT_H */
