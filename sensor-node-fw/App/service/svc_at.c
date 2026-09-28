/*============================================================================
* svc_at.c —— 服务层：AT 指令解析实现
*
* 层次定位：Service 层。纯字符处理，无 I/O、无框架依赖、无存储访问。
*
* 解析结果是一个 `SvcAtCmd`（见 svc_at.h）；「命令怎么执行、回复什么文本」
* 由 App 层决定——参数读写要经 AO_Store 异步落盘，不能在解析器里做。
*
* 命令形式见 svc_at.h 的文件头。新增命令：在下面的 parseLine() 里追加分支，
* 并在 App 层的 App_Command_Dispatch() 里补执行。
*===========================================================================*/
#include "svc_at.h"

#include <string.h>

#if (APP_CFG_AT_ENABLE != 0)

/*--------------------------------------------------------------------------*/
/** @brief 行结束符判定。 */
static bool isLineEnd(uint8_t b) {
    return ((b == (uint8_t)'\r') || (b == (uint8_t)'\n'));
}

/*..........................................................................*/
/** @brief 大小写无关比较（只用于短命令，ASCII）。 */
static bool eqIgnoreCase(char const *a, char const *b) {
    while ((*a != '\0') && (*b != '\0')) {
        char ca = *a;
        char cb = *b;
        if ((ca >= 'A') && (ca <= 'Z')) {
            ca = (char)(ca - 'A' + 'a');
        }
        if ((cb >= 'A') && (cb <= 'Z')) {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return false;
        }
        ++a;
        ++b;
    }
    return ((*a == '\0') && (*b == '\0'));
}

/*..........................................................................*/
/** @brief 是否以 `AT+` 开头（大小写不敏感）。 */
static bool startsWithAtPlus(char const *line) {
    return (line[0] != '\0') && (line[1] != '\0') && (line[2] != '\0') &&
           ((line[0] == 'a') || (line[0] == 'A')) &&
           ((line[1] == 't') || (line[1] == 'T')) &&
           (line[2] == '+');
}

/*..........................................................................*/
/** @brief 就地转小写（参数名统一小写，便于与参数表比较）。 */
static void toLowerInPlace(char *s) {
    while (*s != '\0') {
        if ((*s >= 'A') && (*s <= 'Z')) {
            *s = (char)(*s - 'A' + 'a');
        }
        ++s;
    }
}

/*..........................................................................*/
/** @brief 拷贝一段命令名到 out（带长度上限、保证以 '\0' 结尾）。
* @return false = 名字为空或过长
*/
static bool copyName(char *out, size_t cap, char const *src, size_t len) {
    size_t i;

    if ((len == 0U) || (len >= cap)) {
        return false;
    }
    for (i = 0U; i < len; ++i) {
        out[i] = src[i];
    }
    out[len] = '\0';
    toLowerInPlace(out);
    return true;
}

/*..........................................................................*/
/** @brief 取参数值：允许外层带引号（3GPP 的字符串参数写法），也允许不带。
*
* NOTE (为什么两者都收)：3GPP 规定字符串型参数要加引号（`AT+SN="abc"`），但真实模组
*   普遍容忍不带引号的写法。这里收两种、发的时候一律带引号，兼顾兼容与规范。
*/
static bool copyValue(char *out, size_t cap, char const *src) {
    size_t const len = strlen(src);
    size_t inner;

    if (cap == 0U) {
        return false;
    }
    out[0] = '\0';

    /* 外层成对引号：剥掉 */
    if ((len >= 2U) && (src[0] == '"') && (src[len - 1U] == '"')) {
        inner = len - 2U;
        if ((inner + 1U) > cap) {
            return false;
        }
        (void)memcpy(out, &src[1], inner);
        out[inner] = '\0';
        return (inner > 0U);
    }

    if ((len + 1U) > cap) {
        return false;
    }
    (void)memcpy(out, src, len + 1U);
    return (len > 0U);
}

/*..........................................................................*/
/** @brief 把一整行解析成命令。
* @return true = 识别成功（out->kind 不是 NONE）
*/
static bool parseLine(char const *line, SvcAtCmd *out) {
    size_t const len = strlen(line);
    char const *eq;

    if (len == 0U) {
        return false;       /* 空行：静默（避免刷屏） */
    }

    /* 带 `AT` 前缀的固定命令；`AT+CFG?` / `AT+I2C?` 必须在通用的
    * 「AT+<参数名>?」之前判，否则会被当成"参数名 cfg / i2c"。 */
    if (eqIgnoreCase(line, "AT")) {
        out->kind = SVC_AT_KIND_PING;
        return true;
    }
    if (eqIgnoreCase(line, "AT&F")) {
        out->kind = SVC_AT_KIND_DEFAULTS;
        return true;
    }
    if (eqIgnoreCase(line, "AT+CFG?")) {
        out->kind = SVC_AT_KIND_INFO;
        return true;
    }
    if (eqIgnoreCase(line, "AT+I2C?")) {
        out->kind = SVC_AT_KIND_SCAN;
        return true;
    }
		if(eqIgnoreCase(line, "AT+VER?")){
				out->kind =SVC_AT_KIND_VERSION;
				return true;
		}
		if(eqIgnoreCase(line, "AT+CMD?")){
				out->kind =SVG_AT_KIND_CMD;
				return true;
		}
		/* `AT+REBOOT` / `AT+RST`：重启设备。
		*  NOTE (为什么两个拼写都收)：平台规则文档给设备侧的重启命令名是 **`AT+RST`**
		*    （见 docs_sensor/11-平台接口需求.md 1.4，此前列为"未实现"），而本工程自己的
		*    叫法与 BLE 侧一致、用 `AT+REBOOT`。两者语义相同，合并到同一个 kind，
		*    免得平台发 `AT+RST` 时被当成未知命令回 ERROR。
		*  NOTE (为什么必须放在这里)：这两个拼写既不带 '=' 也不带 '?'，若落到下面的通用
		*    分支之后，会被当成"参数名 rst/reboot"去查参数表而回 +CME ERROR:1。
		*  NOTE (只认裸命令)：不接受 `AT+RST=1` / `AT+REBOOT?`——复位不可逆，形态越少越不
		*    容易误触发（带参数会走 SET/TEST 分支，自然回错误码）。
		*    （BLE 模组那条 `AT+RST=1` 走的是**另一路串口、另一套解析器**，与此无关。） */
		if (eqIgnoreCase(line, "AT+REBOOT") || eqIgnoreCase(line, "AT+RST")) {
				out->kind = SVC_AT_KIND_REBOOT;
				return true;
		}
		/* `AT+BLESVCUUID?`：查 BLE 服务 UUID（= 参数 `uuid`）。
		*  NOTE (为什么单列一条而不是靠通用 `AT+<参数名>?`)：**响应回显的名字不同**。
		*    通用读法回 `+UUID:"…"`，而平台侧要的是 `+BLESVCUUID:"…"`——
		*    它的匹配式是 `^\+BLESVCUUID:[^\r\n]*\r\nOK\r\n$`（2026-09-28 用户给出），
		*    前缀不对就算不上。所以这里给一个专用 kind，由分发器用固定前缀格式化；
		*    `param` 仍填 `"uuid"`，取值走参数表，**名字的唯一真相不搬家**。
		*  NOTE (为什么必须放在通用分支之前)：它带 `?`，落下去会被当成"参数名
		*    blesvcuuid"去查参数表而回 +CME ERROR:1。
		*  NOTE (只认查询形式)：写入仍是 `AT+UUID="…"`（且现在**只允许串口写**）。 */
		if (eqIgnoreCase(line, "AT+BLESVCUUID?")) {
				if (!copyName(out->param, sizeof(out->param), "uuid", 4U)) {
						return false;
				}
				out->kind = SVC_AT_KIND_BLE_UUID;
				return true;
		}
    /* 其余命令一律要求 `AT+` 前缀 */
    if (!startsWithAtPlus(line)) {
        return false;
    }

    /* `AT+<参数名>=<值>` 或 `AT+<参数名>=?`（测试形式） */
    eq = strchr(line, '=');
    if (eq != (char const *)0) {
        size_t const nameLen = (size_t)(eq - (line + 3));

        if (!copyName(out->param, sizeof(out->param), line + 3, nameLen)) {
            return false;
        }
        if ((eq[1] == '?') && (eq[2] == '\0')) {
            out->kind = SVC_AT_KIND_TEST;
            return true;
        }
        if (!copyValue(out->value, sizeof(out->value), eq + 1)) {
            return false;   /* 空值：整行无效，调用方回 ERROR */
        }
        out->kind = SVC_AT_KIND_SET;
        return true;
    }

    /* `AT+<参数名>?` */
    if ((len >= 5U) && (line[len - 1U] == '?')) {
        size_t const nameLen = len - 3U - 1U;   /* 去掉 "AT+" 与结尾的 '?' */

        if (!copyName(out->param, sizeof(out->param), line + 3, nameLen)) {
            return false;
        }
        out->kind = SVC_AT_KIND_GET;
        return true;
    }

    return false;
}

#endif /* APP_CFG_AT_ENABLE */

/*--------------------------------------------------------------------------*/
void SVC_At_Init(SvcAt *me) {
    if (me == (SvcAt *)0) {
        return;
    }
    me->len = 0U;
    me->line[0] = '\0';
}

/*--------------------------------------------------------------------------*/
bool SVC_At_Feed(SvcAt *me, uint8_t byte, SvcAtCmd *out) {
#if (APP_CFG_AT_ENABLE == 0)
    /* 关闭时整体空操作：调用点不需要任何改动 */
    (void)me;
    (void)byte;
    (void)out;
    return false;
#else
    if ((me == (SvcAt *)0) || (out == (SvcAtCmd *)0)) {
        return false;
    }
    out->kind = SVC_AT_KIND_NONE;

    if (isLineEnd(byte)) {
        bool const ok = parseLine(me->line, out);
        me->len = 0U;              /* 一行结束，无论是否识别都清空 */
        me->line[0] = '\0';
        return ok;
    }

    /* 可打印字符才累积；控制字符直接丢弃 */
    if ((byte >= 0x20U) && (byte < 0x7FU)) {
        if (me->len >= (uint8_t)(SVC_AT_LINE_MAX - 1U)) {
            me->len = 0U;          /* 超长：丢弃整行，避免残行误判 */
            me->line[0] = '\0';
            return false;
        }
        me->line[me->len] = (char)byte;
        ++me->len;
        me->line[me->len] = '\0';
        /* NOTE (2026-09-27 删掉的捷径)：这里原本还有一段「缓冲区恰好等于 at 就立即产出
         *   PING」的兼容逻辑，用来支持不带 CR/LF 的裸 `at`。改成 `AT+…` 语法后它会
         *   在输入 `AT+SN?` 的第 2 个字符处误触发并清空缓冲区，**必须去掉**。
         *   现在所有命令都只在一行结束时判定。 */
    }

    return false;
#endif
}
