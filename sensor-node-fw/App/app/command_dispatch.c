/*============================================================================
* command_dispatch.c —— App 层：统一设备命令分发器
*
* 层次定位：**App(应用层)** → Service → Driver → BSP → HAL/CMSIS
*           本文件是「命令模型」与「本工程动作」之间唯一的适配点。
*
* 目标硬件：STM32F103RCT6（LQFP64）
* 所属内核：QP/C 6.9.1 · QV（协作式、非抢占、单栈）
*
* 职责：
*   把一条 `AppCommandRequest`（串口和 4G 送进来的是同一个模型）翻译成本工程的实际动作，
*   并生成统一格式的 AT 响应文本，放进 `AppCommandResult.raw`。
*
* 数据流（串口与 4G 共用中间这一段，这是本文件存在的全部意义）：
*
*   串口：USART1 字节 → svc_at.c 解析 → SvcAtCmd ─┐
*                                                 ├→ AppCommandRequest
*   4G  ：MQTT JSON {id,command} → 解析 → SvcAtCmd ┘          │
*                                                             ↓
*                                             App_Command_Dispatch()（本文件）
*                                                             ↓
*                                      App_Store_* 等本工程动作 / 参数内存影子
*                                                             ↓
*                                              AppCommandResult.raw
*                                         ┌───────────────────┴───────────────────┐
*                                    串口：原样写 LOG_WRITE()          4G：包进 command/reply
*
* NOTE (分层)：解析在 Service 层（`App/service/svc_at.c`），请求/结果模型在
*   `App/app_command/`，而**动作在本文件**。`App/app_command/` 不允许访问 Store、QP 或硬件
*   ——它要能整个目录搬走；本文件恰好相反，它就是「这套命令在本工程里到底做什么」的地方，
*   所以允许调用 `App_Store_*` 与 `app_config.h`。Service 层不得反向调用本文件。
*
* NOTE (不阻塞)：写参数只改内存影子并置 dirty，真正的落盘由 `AO_Store` 异步完成，
*   因此本文件每个分支都「立刻返回」——回 `OK` 表示「已受理」，**不代表已经写进 EEPROM**。
*   落盘证据是随后日志里的 `store: saved ver=… seq=… slot=…`。QV 单栈协作，这里一旦阻塞
*   全系统停摆，所以本文件里不允许出现延时、等总线、忙等。
*
* NOTE (响应形态，V.250 / 3GPP 27.007)：
*   - 成功一律大写 `OK`（不是小写 `ok`）
*   - 读字符串参数回 `+<NAME>:"<值>"`；读**数值型**参数回 `+<NAME>:<值>`（不带引号，
*     当前只有 `TRANSINTVL`）
*   - 测试形式回 `+<NAME>:(<最大长度>)`；数值型参数回**允许范围** `+<NAME>:(<min>,<max>)`
*   - 查状态回**位置式** `+CFG:<8 个逗号分隔字段>`（不是 `key=value` 自造格式）
*   - 失败回 `+CME ERROR:<n>`
*
* NOTE (2026-09-27 统一收尾 `OK`)：读参数 / 测试 / `AT+CFG?` / `AT+VER?` 这几类响应
*   在参数行之后**追加最终 `OK\r\n`**，与写命令的响应形态对齐。这样串口与 4G 拿到的是同一串
*   `raw` 文本：串口原样打印，4G 直接把它当 `data` 包进 `command/reply`，不必为两种来源
*   各写一套格式化。协议表见 docs_sensor/06-协议格式.md。
*===========================================================================*/

#include "command_dispatch.h"

#include "app_config.h"
#include "store.h"
#include "system.h"
#include "svc_log.h"

#include <stdio.h>
#include <string.h>

/*--------------------------------------------------------------------------*/
/**
 * @brief 把一段固定文本装进统一结果。
 *
 * @param result    出参：命令结果（由调用方在栈上给出，本文件不持有状态）
 * @param status    成功/失败状态，写入 `result->status`
 * @param errorCode 供 4G 侧透传的错误码，写入 `result->error_code`
 * @param text      完整响应文本（含行尾 `\r\n`，可含最终 `OK\r\n`）
 * @return true = 已装入；false = 参数为空或文本放不下
 *
 * NOTE (缓冲区归属)：`result->raw` 属于调用方，容量是 `SVC_AT_RESP_MAX`。
 *   放不下时**清空 raw 并返回 false**，绝不截断成半条响应——半条响应比不回复更难查，
 *   而且串口那边会把半条文本照原样发出去。
 */
static bool putText(AppCommandResult *result,
                    AppCommandResultStatus status,
                    uint16_t errorCode,
                    char const *text) {
    size_t length;

    if ((result == (AppCommandResult *)0) ||
        (text == (char const *)0)) {
        return false;
    }

    length = strlen(text);
    if (length >= sizeof(result->raw)) {
        result->status = APP_COMMAND_RESULT_ERROR;
        result->error_code = 0U;
        result->raw[0] = '\0';
        return false;
    }

    (void)memcpy(result->raw, text, length + 1U);
    result->status = status;
    result->error_code = errorCode;
    return true;
}

/*--------------------------------------------------------------------------*/
/**
 * @brief 检查 `snprintf()` 的返回值并标记命令完成。
 *
 * @param result   出参：命令结果
 * @param produced `snprintf()` 的返回值（**本该写入的长度**，不是实际写入的长度）
 * @return true = 文本完整无截断；false = 溢出，已改写为 `ERROR\r\n`
 *
 * NOTE (为什么必须查返回值)：`snprintf()` 返回「假如空间够会写多少字节」，
 *   因此 `produced >= 容量` 就说明**被截断了**；只看它有没有报错是不够的。
 *   截断的响应会让上位机解析出半条参数值，属于必须挡住的故障。
 */
static bool finishFormatted(AppCommandResult *result, int produced) {
    if ((produced < 0) ||
        ((size_t)produced >= sizeof(result->raw))) {
        return putText(result,
                       APP_COMMAND_RESULT_ERROR,
                       0U,
                       "ERROR\r\n");
    }

    result->status = APP_COMMAND_RESULT_DONE;
    result->error_code = 0U;
    return true;
}

/*--------------------------------------------------------------------------*/
/**
 * @brief 把参数名转成大写写进 out。
 *
 * @param out  目标缓冲（由调用方在栈上给出）
 * @param cap  out 的容量（含结尾 '\0'）
 * @param name 小写参数名（来自 Service 层解析结果）
 * @return true = 已写入；false = 放不下
 *
 * NOTE (为什么要转大写)：3GPP 27.007 的响应里参数名一律大写，而 svc_at.c 解析时
 *   为了大小写不敏感已把 `param` 统一成小写。响应中的大小写是协议的一部分，
 *   上位机（含本工程自己的 at_smoke.ps1）按 `^\+SN:"` 这种大写形式匹配，
 *   因此在这里统一还原，而不是依赖输入的大小写。
 */
static bool nameUpper(char *out, size_t cap, char const *name) {
    size_t i;

    for (i = 0U; name[i] != '\0'; ++i) {
        if ((i + 1U) >= cap) {
            return false;
        }

        out[i] = ((name[i] >= 'a') && (name[i] <= 'z'))
                 ? (char)(name[i] - 'a' + 'A')
                 : name[i];
    }

    out[i] = '\0';
    return true;
}

/*--------------------------------------------------------------------------*/
/**
 * @brief 该参数项是否为「数值型」参数（响应不带引号、写入要按范围校验）。
 *
 * @param id `SVC_Param_Find()` 的结果（可为负 = 没找到）
 *
 * NOTE (为什么按 id 判而不是按名字)：参数名的唯一真相在 `App/service/svc_param.c`
 *   的参数表里；这里只标记「这一个 id 的读写形态与字符串参数不同」，不再抄一遍名字。
 * NOTE (当前只有一个)：`transintvl`（周期上报间隔，分钟）。将来再加数值型业务参数时，
 *   把范围校验抽成一张 {min,max} 表即可，形态分支本身不用改。
 */
static bool isNumericParam(int16_t id) {
    return id == (int16_t)SVC_PARAM_TRANSINTVL;
}

/*--------------------------------------------------------------------------*/
/**
 * @brief 这条参数是不是「身份类」参数（只允许串口写、改完要重启）。
 *
 * @param id 参数 id（`SVC_Param_Find()` 的结果，可为负 = 没找到）
 *
 * NOTE (当前两个，2026-09-28)：
 *   - `sn`（设备序列号）：云平台主题里的 `deviceId` + 蓝牙认证应答里的 `sn`；
 *   - `uuid`（BLE 服务 UUID）：**每台唯一**，启动时写进 VG6328，手机小程序靠它认设备。
 *   两者都是"产线写一次、之后不该被远程改"的东西，所以同一套待遇：
 *   **仅串口可写 + 改完自动重启**（UUID 本来就只在上电时写进模组，不重启不生效）。
 * NOTE (加新参数时)：往 `kIdentityParams[]` 追加一个 id 即可，
 *   写入权限、变更判定与自动重启三处会一起生效。
 * NOTE (值校验**不**在这里)：只有会嵌进 MQTT 主题的 `SN` 才走设备号规则，
 *   见下面的 `isDeviceIdParam()`——两件事分开，别合。
 */
static bool isIdentityParam(int16_t id) {
    /*! 身份类参数 id 表。加新参数时在这里补一行。 */
    static uint8_t const kIdentityParams[] = {
        (uint8_t)SVC_PARAM_BLE_SN,      /* 设备序列号 */
        (uint8_t)SVC_PARAM_BLE_UUID     /* BLE 服务 UUID */
    };
    uint8_t i;

    if (id < 0) {
        return false;
    }
    for (i = 0U; i < (uint8_t)(sizeof(kIdentityParams) / sizeof(kIdentityParams[0])); ++i) {
        if ((uint8_t)id == kIdentityParams[i]) {
            return true;
        }
    }
    return false;
}

/*--------------------------------------------------------------------------*/
/**
 * @brief 这条参数的**值**是否要按「设备号」规则校验（字符集/长度/出厂哨兵）。
 *
 * @param id 参数 id
 *
 * NOTE (为什么与 `isIdentityParam()` 分开，2026-09-28)：两者**不是一回事**——
 *   `SN` 的值会嵌进 MQTT 主题（`sensor-product/<SN>/command`），所以字符集、长度、
 *   "是否仍是出厂默认值"都必须管；`UUID` 虽然同样是身份类，但它的值是一段 36 字符
 *   的服务 UUID，规则由 BLE 侧决定，**不能**套设备号那套判据：
 *   `SVC_Param_CheckDeviceId()` 会把"等于出厂默认 UUID"判成"产线没刷"而拒写——
 *   而那个默认值恰恰是合法值。
 *   所以：**只有 `SN` 走设备号校验**；将来有新的"会进主题"的参数，在这里补一行。
 */
static bool isDeviceIdParam(int16_t id) {
    return id == (int16_t)SVC_PARAM_BLE_SN;
}

/*--------------------------------------------------------------------------*/
/**
 * @brief 这条参数是否允许从该来源写入。
 *
 * @param id     参数 id（`SVC_Param_Find()` 的结果，可为负 = 没找到）
 * @param source 命令来源（`AppCommandRequest.source`）
 * @return false = 该来源无权写这条参数（调用方回 `+CME ERROR:4`）
 *
 * NOTE (为什么需要它，2026-09-28)：`SN` 这类**身份参数**只在产线上写一次，
 *   之后云端与小程序都不该能改。原因有两层：
 *     ① 平台侧一旦被误操作/被盗用，就能**批量改掉设备身份**；
 *     ② `SN` 是设备身份（MQTT 主题里的 deviceId、蓝牙认证应答里的 sn），
 *        改错等于「设备连到别人的主题上」，而且是静默的（见 [[08-改动日志]] `#51`）。
 *   所以权限按**来源**判：身份参数只允许**串口**（= 必须物理接触本机，产线刷写），
 *   云端只能读（`AT+SN?` 不受影响）。
 *
 * NOTE (为什么按 id 判、且表放在本文件)：与 `isNumericParam()` 同一套做法——
 *   参数名的唯一真相在 `App/service/svc_param.c` 的参数表里，这里只标「哪些 id 是身份类」。
 *   权限是**本工程的策略**，且必须与 App 层的来源枚举对齐，所以不放进 Service 层
 *   （Service 层要保持「只描述参数长什么样」）。
 *
 * NOTE (BLE 侧)：小程序的命令走 `ble_protocol.c`，那里只能改 5 个测量参数，
 *   本来就碰不到参数表；若将来开放参数写入，必须复用本函数判权。
 */
static bool writeAllowedFrom(int16_t id, AppCommandSource source) {
    if (source == APP_COMMAND_SOURCE_SERIAL) {
        return true;    /* 串口是产线入口：任何参数都可写 */
    }
    if (id < 0) {
        /* 参数名不认识：交给后面的分支回 `+CME ERROR:1`，别在这里抢答成 4 */
        return true;
    }
    return !isIdentityParam(id);
}

/*--------------------------------------------------------------------------*/
/**
 * @brief 执行一条统一设备命令（串口与 4G 共用）。
 *
 * @param request 输入：统一命令请求（来源 + `SvcAtCmd` + 可选关联 ID）
 * @param result  输出：统一命令结果（状态 + 错误码 + AT 原始响应）
 * @return true  = **本分发器认识这条命令**，`result` 已填好（失败命令同样返回 true，
 *                 此时 `result->status` 为 `APP_COMMAND_RESULT_ERROR`，`raw` 里是错误文本）；
 *         false = 本分发器不处理该命令，调用方自行兜底。
 *
 * NOTE (返回值语义，容易踩)：`true` 只表示「命令已被识别」，**不表示执行成功**。
 *   调用方要判成败请看 `result->status`，不要看返回值。反过来，当前 `parseLine()` 能
 *   解析出的所有 kind 在这里都有分支，`default: return false` 只是防御性兜底
 *   （例如将来 Service 层先加了枚举、本文件还没补分支时，串口会走「未处理」路径）。
 *
 * NOTE (无状态)：本函数不使用任何静态变量，可安全地被串口和 4G 两条路径先后调用；
 *   QV 单栈运行到完成，`result` 归调用方所有，因此不需要加锁，也不能保存指针留到以后用。
 */
bool App_Command_Dispatch(AppCommandRequest const *request,
                          AppCommandResult *result) {
    int16_t id;
    int produced;

    if ((request == (AppCommandRequest const *)0) ||
        (result == (AppCommandResult *)0)) {
        return false;
    }

    /* 先把结果初始化成「失败且无文本」：任何中途返回都不会给调用方留下上次的旧内容 */
    result->status = APP_COMMAND_RESULT_ERROR;
    result->error_code = 0U;
    result->raw[0] = '\0';

    switch (request->command.kind) {
        /* `AT` 握手：只证明通路活着，没有任何副作用 */
        case SVC_AT_KIND_PING:
            return putText(result,
                           APP_COMMAND_RESULT_DONE,
                           0U,
                           "OK\r\n");

        /* `AT+BLESVCUUID?` 查 BLE 服务 UUID（= 参数 `uuid`，2026-09-28 新增）。
         *
         * NOTE (为什么单列一条)：它和通用读法读的是**同一个值**，但**回显名不同**——
         *   平台侧的匹配式是 `^\+BLESVCUUID:[^\r\n]*\r\nOK\r\n$`（用户给出的原文），
         *   前缀写成 `+UUID:` 就算不上。所以前缀在这里写死，取值仍走参数表
         *   （`request->command.param` = "uuid"，名字的唯一真相不搬家）。
         * NOTE (为什么紧跟通用 GET 之后)：两者形态一致，放一起便于对照维护。 */
        case SVC_AT_KIND_BLE_UUID: {
            id = SVC_Param_Find(request->command.param);
            if (id < 0) {
                /* 参数表里没有 `uuid` 才算异常（正常不可能走到这里） */
                return putText(result, APP_COMMAND_RESULT_ERROR, 1U, "+CME ERROR:1\r\n");
            }
            produced = snprintf(result->raw, sizeof(result->raw),
                                "+BLESVCUUID:\"%s\"\r\nOK\r\n",
                                App_Store_Value((uint8_t)id));
            return finishFormatted(result, produced);
        }

        /* `AT+<参数名>?` 读参数：值取自 AO_Store 的 RAM 影子，**不访问 I2C 总线**，
         * 因此这里不会阻塞，也不受 EEPROM 忙闲影响 */
        case SVC_AT_KIND_GET: {
            char name[SVC_AT_NAME_MAX];

            id = SVC_Param_Find(request->command.param);
            if ((id < 0) || SVC_Param_IsRetired((uint8_t)id)) {
                /* 参数名不在参数表之列（当前 7 个），或者是**已退役**的槽位（`name`：
                 * 广播名已并入 `SN`，见 svc_param.h 的 NOTE）——对调用方一律是
                 * 「没有这个名字」，按 AT 惯例回 +CME ERROR:1。
                 * NOTE (为什么不回一个专门的码)：退役是**本工程的内部沿革**，上位机不该
                 *   为此写分支；对它们来说这个名字从来不存在。要读广播名就查 `AT+SN?`。 */
                return putText(result,
                               APP_COMMAND_RESULT_ERROR,
                               1U,
                               "+CME ERROR:1\r\n");
            }

            if (!nameUpper(name,
                           sizeof(name),
                           SVC_Param_Name((uint8_t)id))) {
                return putText(result,
                               APP_COMMAND_RESULT_ERROR,
                               0U,
                               "ERROR\r\n");
            }

            /* 数值型参数回**不带引号**的 `+TRANSINTVL:20`；字符串型参数回
             * `+SN:"…"`。两者的区别是协议的一部分，别为了「统一」把引号去掉。 */
            if (isNumericParam(id)) {
                produced = snprintf(result->raw,
                                    sizeof(result->raw),
                                    "+%s:%s\r\nOK\r\n",
                                    name,
                                    App_Store_Value((uint8_t)id));
            } else {
                produced = snprintf(result->raw,
                                    sizeof(result->raw),
                                    "+%s:\"%s\"\r\nOK\r\n",
                                    name,
                                    App_Store_Value((uint8_t)id));
            }

            return finishFormatted(result, produced);
        }

        /* `AT+<参数名>=?` 测试形式：只报告「最多能写多少个字符」，不读也不写 */
        case SVC_AT_KIND_TEST: {
            char name[SVC_AT_NAME_MAX];

            id = SVC_Param_Find(request->command.param);
            if ((id < 0) || SVC_Param_IsRetired((uint8_t)id)) {
                /* 与读取分支同一判据：退役的 `name` 对外不存在（广播名 = `SN`） */
                return putText(result,
                               APP_COMMAND_RESULT_ERROR,
                               1U,
                               "+CME ERROR:1\r\n");
            }

            if (!nameUpper(name,
                           sizeof(name),
                           SVC_Param_Name((uint8_t)id))) {
                return putText(result,
                               APP_COMMAND_RESULT_ERROR,
                               0U,
                               "ERROR\r\n");
            }

            /* 数值型参数报的是**允许范围** `+TRANSINTVL:(1,20)`；字符串型参数报的是
             * 可写字符数。这里若跟着报容量 `(3)`，上位机会以为「能写 3 个字符」。 */
            if (isNumericParam(id)) {
                produced = snprintf(result->raw,
                                    sizeof(result->raw),
                                    "+%s:(%u,%u)\r\nOK\r\n",
                                    name,
                                    (unsigned)APP_CFG_MODEM_TRANS_INTERVAL_MIN,
                                    (unsigned)APP_CFG_MODEM_TRANS_INTERVAL_MAX);
            } else {
                /* NOTE (减 1 的原因)：`SVC_Param_Cap()` 是缓冲容量（含结尾 '\0'），
                 * 可写字符数是容量 − 1。回错这个数会让上位机按错误长度截断 SN。 */
                produced = snprintf(result->raw,
                                    sizeof(result->raw),
                                    "+%s:(%u)\r\nOK\r\n",
                                    name,
                                    (unsigned)(SVC_Param_Cap((uint8_t)id) - 1U));
            }

            return finishFormatted(result, produced);
        }

        /* `AT+<参数名>="<值>"` 写参数：立即改内存影子并置 dirty，落盘异步。
         * 错误码按 Service 层的状态码逐条映射，不做「失败」二字了事。 */
        case SVC_AT_KIND_SET: {
            int16_t const paramId = SVC_Param_Find(request->command.param);
            bool identityChanged = false;

            /* 退役槽位（`name`）排在最前：它对外**不存在**，所以既不是「越权」（4）也不是
             * 「值非法」（2），而是「没有这个参数名」（1）——与读取分支同一口径。
             * NOTE (为什么必须在权限判断之前)：`name` 曾经是可写的普通参数，若晚一步判，
             *   串口那一侧（任何参数都可写）会**真的把它写进 EEPROM**，退役就成了空话。 */
            if (SVC_Param_IsRetired((uint8_t)paramId)) {
                return putText(result,
                               APP_COMMAND_RESULT_ERROR,
                               1U,
                               "+CME ERROR:1\r\n");
            }

            /* 来源权限：身份类参数（当前 `SN`）只允许串口写，云端只能读。
             * NOTE (为什么排在最前)：越权与「值非法」是两回事——先判权限，
             *   免得越权写一个格式也不对的值时回一个误导性的错误码。
             * NOTE (只打参数名与来源)：命令原文可能带引号值（别的参数是凭据），
             *   这里按 AGENTS 4.2 的凭据纪律，只记"谁想改哪个参数、被拒了"。 */
            if (!writeAllowedFrom(paramId, request->source)) {
                LOG_W("cmd: write to %s refused for this source",
                      SVC_Param_Name((uint8_t)paramId));
                return putText(result,
                               APP_COMMAND_RESULT_ERROR,
                               4U,
                               "+CME ERROR:4\r\n");
            }

            /* 身份参数还要过**值**校验（2026-09-28）：这个值会嵌进 MQTT 主题，
             * 空串 / 含 `/` `+` `#` 空格等非法字符 / 仍是出厂默认值时一律拒写，
             * 免得脏值进 EEPROM——那种情况下设备会"订阅成功但永远收不到消息"，
             * 而且**是静默的**（与 `#51` 同类）。判据与 4G 启动时用的是同一个函数。
             * NOTE (日志必须带原因)：判据上线当天就被自己坑了一次——只打"被拒"看不出
             *   到底哪里不合法（空？非法字符？长度？），现场只能猜。所以这里带上
             *   原因码、出错下标与那一个字节（十六进制），**仍不打印值本身**。 */
            if (isDeviceIdParam(paramId)) {
                uint8_t badIndex = 0U;
                SvcDeviceIdStatus const idStatus =
                    SVC_Param_CheckDeviceId(request->command.value, &badIndex);

                if (idStatus != SVC_DEVICE_ID_OK) {
                    LOG_W("cmd: %s rejected reason=%u pos=%u ch=0x%02X len=%u",
                          SVC_Param_Name((uint8_t)paramId),
                          (unsigned)idStatus,
                          (unsigned)badIndex,
                          (unsigned)(uint8_t)request->command.value[badIndex],
                          (unsigned)strlen(request->command.value));
                    return putText(result,
                                   APP_COMMAND_RESULT_ERROR,
                                   2U,
                                   "+CME ERROR:2\r\n");
                }
            }

            /* 数值型参数先做范围校验再落盘：越界回 `+CME ERROR:2`（与「值非法」同档），
             * 且**不把非法值写进影子**——写进去会让 `AT+CFG?` 出现 dirty、EEPROM 里
             * 留下一个下次上电就被当成真值的垃圾数。 */
            if (isNumericParam(paramId)) {
                uint32_t minutes;
                char canonical[8];

                if (!SVC_Param_TextToU32(request->command.value, &minutes) ||
                    (minutes < (uint32_t)APP_CFG_MODEM_TRANS_INTERVAL_MIN) ||
                    (minutes > (uint32_t)APP_CFG_MODEM_TRANS_INTERVAL_MAX)) {
                    return putText(result,
                                   APP_COMMAND_RESULT_ERROR,
                                   2U,
                                   "+CME ERROR:2\r\n");
                }

                /* 归一化后再存：`AT+TRANSINTVL=05` 存成 "5"，读回来与 `=?` 报的范围自洽 */
                produced = snprintf(canonical, sizeof(canonical), "%lu",
                                    (unsigned long)minutes);
                if ((produced < 0) || ((size_t)produced >= sizeof(canonical))) {
                    return putText(result,
                                   APP_COMMAND_RESULT_ERROR,
                                   0U,
                                   "ERROR\r\n");
                }

                if (App_Store_SetName(
                        SVC_Param_Name((uint8_t)SVC_PARAM_TRANSINTVL),
                        canonical) != SVC_OK) {
                    /* 走到这里只可能是「存储还没就绪」——值本身已经校验过了。
                     * 与字符串参数共用同一套错误码口径：未就绪 = `+CME ERROR:3`。 */
                    return putText(result,
                                   APP_COMMAND_RESULT_ERROR,
                                   3U,
                                   "+CME ERROR:3\r\n");
                }

                return putText(result,
                               APP_COMMAND_RESULT_DONE,
                               0U,
                               "OK\r\n");
            }

            /* 写**之前**先记下「值是不是真的变了」：身份参数改完要自动重启，而产线脚本
             * 会重复跑（幂等），值没变还重启就成了反复重启。
             * NOTE (必须在这里判)：`App_Store_SetName()` 一执行影子就是新值了，
             *   在它之后再比较只会永远相等，重启也就永远不触发。 */
            if (isIdentityParam(paramId)) {
                identityChanged =
                    (strcmp(App_Store_Value((uint8_t)paramId),
                            request->command.value) != 0);
            }

            switch (App_Store_SetName(request->command.param,
                                       request->command.value)) {
                case SVC_OK:
                    /* 身份参数写成功 → 请 AO_System 复位，让新身份在下一次上电时**整体**生效。
                     *
                     * NOTE (为什么必须重启)：`SN` 是设备身份。MQTT 的 `clientId` 与全部主题是
                     *   连接计划启动时拼好的，蓝牙那边也有已认证的会话——只改内存影子不会
                     *   重新订阅、也不会重走蓝牙配置，设备会停在"旧身份"的世界里。
                     * NOTE (为什么这里只投请求)：复位由 `AO_System` 延时执行，它会先等
                     *   `App_Store_IsIdle()`（参数真的落盘）再复位；否则重启回来还是旧值。
                     *   详见 `App/app/system.c` 的文件头。
                     * NOTE (来源)：写身份参数只可能来自串口（云端在 `writeAllowedFrom()` 就被挡了），
                     *   但来源仍单独记成 `identity`，好让现场一眼看出"是固件自己决定重启的"。
                     * NOTE (为什么这里要打日志，2026-09-28 当天补)：用户遇到"回了 OK 但没重启"，
                     *   而**值没变不重启是设计行为**（产线脚本会重复跑）。这条岔口必须一眼可辨，
                     *   否则只能靠猜。`LOG_I` 级别，正式版也留着——它只在上电配置期出现一次。 */
                    if (identityChanged) {
                        LOG_I("cmd: %s changed -> reboot to apply new identity",
                              SVC_Param_Name((uint8_t)paramId));
                        App_System_RequestReboot(APP_REBOOT_SOURCE_IDENTITY);
                    }
                    else {
                        LOG_I("cmd: %s unchanged -> no reboot (idempotent)",
                              SVC_Param_Name((uint8_t)paramId));
                    }

                    return putText(result,
                                   APP_COMMAND_RESULT_DONE,
                                   0U,
                                   "OK\r\n");

                case SVC_ERR_PARAM:
                    return putText(result,
                                   APP_COMMAND_RESULT_ERROR,
                                   1U,
                                   "+CME ERROR:1\r\n");

                case SVC_ERR_RANGE:
                    /* 值为空或超长（超过该字段容量） */
                    return putText(result,
                                   APP_COMMAND_RESULT_ERROR,
                                   2U,
                                   "+CME ERROR:2\r\n");

                case SVC_ERR_STATE:
                    /* 参数存储还没就绪：上电加载未结束，此时改会被随后的读取覆盖 */
                    return putText(result,
                                   APP_COMMAND_RESULT_ERROR,
                                   3U,
                                   "+CME ERROR:3\r\n");

                default:
                    return putText(result,
                                   APP_COMMAND_RESULT_ERROR,
                                   0U,
                                   "ERROR\r\n");
            }
        }

        /* `AT+CFG?` 存储运行状态：字段是**位置式**的，顺序一旦发布就不能改 */
        case SVC_AT_KIND_INFO: {
            AppStoreInfo info;

            App_Store_Info(&info);

            /* NOTE (字段顺序固定)：rdy,present,loaded,dirty,ver,seq,ok,fail。
             * 只允许在表尾追加新字段；中间插一个会让所有上位机解析错位。
             * rdy 用 resolved（启动加载阶段是否结束），其余三项是布尔量转 0/1。 */
            produced = snprintf(
                result->raw,
                sizeof(result->raw),
                "+CFG:%u,%u,%u,%u,%u,%u,%lu,%lu\r\nOK\r\n",
                (unsigned)(info.resolved ? 1U : 0U),
                (unsigned)(info.present ? 1U : 0U),
                (unsigned)(info.loaded ? 1U : 0U),
                (unsigned)(info.dirty ? 1U : 0U),
                (unsigned)info.config_version,
                (unsigned)info.slot_seq,
                (unsigned long)info.saved,
                (unsigned long)info.failed);

            return finishFormatted(result, produced);
        }

        /* `AT&F`：全部恢复编译期默认值。这也是一次变更，会让 config_version 继续自增
         * （ver 只增不减），不会倒回 0 */
        case SVC_AT_KIND_DEFAULTS:
            if (!App_Store_ResetDefaults()) {
                return putText(result,
                               APP_COMMAND_RESULT_ERROR,
                               3U,
                               "+CME ERROR:3\r\n");
            }

            return putText(result,
                           APP_COMMAND_RESULT_DONE,
                           0U,
                           "OK\r\n");

        /* `AT+VER?` 读固件版本与编译日期：两个宏都来自 app_config.h，
         * 与启动横幅里打的那份是同一来源，避免两处版本号不一致 */
        case SVC_AT_KIND_VERSION:
            produced = snprintf(result->raw,
                                sizeof(result->raw),
                                "+VER:\"%s\",\"%s\"\r\nOK\r\n",
                                APP_CFG_FW_VERSION,
                                APP_CFG_FW_BUILD_DATE);

            return finishFormatted(result, produced);
        /* `AT+CAD?` 支持的 AT 命令表：
         *   +CAD:<固定命令数>,"<命令>",…,<参数个数>,"<参数名>",…\r\nOK\r\n
         * NOTE (为什么循环生成参数名)：参数名的唯一真相是 SVC_Param_* 参数表，
         *   不在这里再抄一遍——加参数时才不会漏改。
         * NOTE (缓冲)：整行约 118 字节，SVC_AT_RESP_MAX 已按它抬到 192；
         *   这里仍逐段查 snprintf 返回值，放不下就回 ERROR，绝不截断。 */
        case SVG_AT_KIND_CMD: {
            static char const kFixed[] =
                "\"AT\",\"AT&F\",\"AT+CFG?\",\"AT+I2C?\",\"AT+VER?\",\"AT+CAD?\",\"AT+RST\",\"AT+BLESVCUUID?\"";
            size_t used;
            uint8_t id;
            uint8_t exposed = 0U;

            /* NOTE (为什么要**先数一遍**，2026-09-28)：参数个数不是 `SVC_PARAM_COUNT`——
             *   退役槽位（`name`）仍留在参数表里（介质兼容，见 svc_param.h），但它对外不存在。
             *   若这里报 8 个、循环里又把它列出来，平台就会照着这张表发 `AT+NAME="…"`，
             *   然后收到 `+CME ERROR:1`：**我们自己的能力清单把自己说错了**。
             *   判据用同一个 `SVC_Param_IsRetired()`，清单与读写测试三处的口径因此一致。 */
            for (id = 0U; id < (uint8_t)SVC_PARAM_COUNT; ++id) {
                if (!SVC_Param_IsRetired(id)) {
                    ++exposed;
                }
            }

            produced = snprintf(result->raw, sizeof(result->raw),
                                "+CAD:8,%s,%u",
                                kFixed, (unsigned)exposed);
            if ((produced < 0) || ((size_t)produced >= sizeof(result->raw))) {
                return putText(result, APP_COMMAND_RESULT_ERROR, 0U, "ERROR\r\n");
            }
            used = (size_t)produced;

            for (id = 0U; id < (uint8_t)SVC_PARAM_COUNT; ++id) {
                char name[SVC_AT_NAME_MAX];

                if (SVC_Param_IsRetired(id)) {
                    continue;   /* 退役参数不进能力清单 */
                }
                if (!nameUpper(name, sizeof(name), SVC_Param_Name(id))) {
                    return putText(result, APP_COMMAND_RESULT_ERROR, 0U, "ERROR\r\n");
                }
                produced = snprintf(&result->raw[used], sizeof(result->raw) - used,
                                    ",\"%s\"", name);
                if ((produced < 0) ||
                    ((size_t)produced >= (sizeof(result->raw) - used))) {
                    return putText(result, APP_COMMAND_RESULT_ERROR, 0U, "ERROR\r\n");
                }
                used += (size_t)produced;
            }

            /* 收尾 `\r\n` + 最终 `OK`；7 字节含结尾 '\0' */
            if ((used + 7U) > sizeof(result->raw)) {
                return putText(result, APP_COMMAND_RESULT_ERROR, 0U, "ERROR\r\n");
            }
            (void)memcpy(&result->raw[used], "\r\nOK\r\n", 7U);
            result->status = APP_COMMAND_RESULT_DONE;
            result->error_code = 0U;
            return true;
        }
        /* `AT+I2C?` 扫描总线：本响应只是「已受理」。
         * NOTE (为什么扫描结果不走这里)：一次扫描要跨十几拍总线，结果由 AO_Store
         * 逐条以 `+I2C:<地址>` 日志上报，末尾 `+I2C:END`（见 store.c）。若在这里等结果，
         * 就违反了「状态处理函数必须尽快返回」。 */
        case SVC_AT_KIND_SCAN:
            if (!App_Store_RequestScan()) {
                /* 已有扫描在跑，或存储未就绪 / 已关闭 */
                return putText(result,
                               APP_COMMAND_RESULT_ERROR,
                               5U,
                               "+CME ERROR:5\r\n");
            }

            return putText(result,
                           APP_COMMAND_RESULT_DONE,
                           0U,
                           "OK\r\n");

        /* `AT+REBOOT`：重启设备。
         *
         * NOTE (为什么这里**只置请求、绝不复位**，2026-09-28)：本函数是"受理"，
         *   不是"执行完"。此刻三样东西都还没落地——
         *     ① 这条 OK 还在异步日志/上行队列里（4G 那边连 `command/reply` 都还没组包）；
         *     ② 刚改过的参数可能还在 AO_Store 的 SAVE 相位上；
         *     ③ 串口发送环里可能还有前面的输出。
         *   就地复位 = 平台判"指令超时"、串口什么都不显示、改的参数丢掉，三样一起发生。
         *   所以真正的复位交给 AO_System 延时执行，判据与兜底见 App/app/system.c。
         *
         * NOTE (回 OK 的语义)：表示"已受理，马上重启"。它满足平台规则 3.3
         *   （`data` 以最终 `OK\r\n` 结尾），平台会先收到成功回复，随后看到设备上下线。 */
        case SVC_AT_KIND_REBOOT:
            App_System_RequestReboot(
                (request->source == APP_COMMAND_SOURCE_MODEM)
                    ? APP_REBOOT_SOURCE_CLOUD
                    : APP_REBOOT_SOURCE_SERIAL);

            return putText(result,
                           APP_COMMAND_RESULT_DONE,
                           0U,
                           "OK\r\n");

        /* 防御性兜底：Service 层新加了 kind 而这里还没补分支时，串口会回 ERROR。
         * 新增命令时改这里，不要改调用方（见 docs_sensor/06-协议格式.md）。 */
        default:
            return false;
    }
}
