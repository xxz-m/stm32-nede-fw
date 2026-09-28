/*============================================================================
* svc_param.h —— 服务层：设备参数记录（编解码 / 校验 / 默认值）
*
* 层次定位：App(应用层) → **Service(服务层)** → Driver → BSP → HAL/CMSIS
*
* 解决什么问题：本工程要掉电保存的设备身份参数（BLE 串号、名称、PIN、Service UUID、
* 设备名、型号、批次）需要在「一块不一定可靠的介质」上存住，并且能判断出
* 「空的 / 坏的 / 旧版本 / 好的」。本模块只管**记录格式**，不管介质是什么：
*
*   记录布局（全部小端，字段变长存储——只存实际长度，因此记录通常只有 110 B 左右）
*
*   +--------+------+------+--------+-------+-------+---------+-----+
*   | magic  | fmt  | seq  | ver    | flags | fc    | rec_len | ... |  头 12 B
*   | 4 B    | 1 B  | 1 B  | 2 B    | 1 B   | 1 B   | 2 B     |     |
*   +--------+------+------+--------+-------+-------+---------+-----+
*   | 字段区：[id 1 B][len 1 B][值 len B] × fc
*   +---------------------------------------------------------------+
*   | CRC16 2 B（覆盖 magic..字段区末尾）                            |
*   +---------------------------------------------------------------+
*
*   magic      = "ATP1"（'A','T','P','1'），用来区分空白介质与有效记录
*   fmt        = 记录格式版本，将来加字段/改语义时靠它做迁移
*   seq        = 槽序号，每次保存自增；双槽（A/B）靠它判断哪份更新（回绕安全比较）
*   ver        = config_version，每次成功保存自增（业务侧用来核对「参数是否真的生效」）
*   flags      = bit0 表示「用户写入过」，其余保留
*   fc/rec_len = 字段个数 / 记录总长度（含头与 CRC），长度不符直接判坏
*
* 约定（见 docs_sensor/07-接口约定.md#Service 层）：
*   - 无状态：所有状态放在调用方持有的 `SvcParam` 里，本模块不定义全局变量
*   - 缓冲区由调用方提供：`out` + `cap`，不足返回 SVC_ERR_NOMEM，不做部分写入
*   - 不碰 HAL、不碰总线、不 #include "qpc.h"：介质读写由 Driver 层负责
*   - CRC 复用 `SVC_Frame_Crc16()`（同一个 CRC-16/MODBUS 实现，不重复造）
*
* 加字段的步骤：① 在 app_config.h（或本文件）给容量宏；② `SvcParamId` 表尾加一项；
* ③ `l_desc[]` 表尾加一行（名字 / 容量 / 默认值宏）。旧记录缺这个字段时自动落到默认值，
* fmt 版本号只在语义变化时才需要动。
*===========================================================================*/
#ifndef SVC_PARAM_H
#define SVC_PARAM_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "app_config.h"
#include "svc_frame.h"      /* svc_status_t 与 SVC_Frame_Crc16() */

/* 参数项（顺序即记录里的字段顺序；增删请只在表尾追加） ----------------------*/
/*! NOTE (记录不只是蓝牙参数，2026-09-28)：前 7 项是 BLE 身份参数（历史命名保留
*   `BLE_` 前缀），第 8 项起是**设备业务参数**。记录格式对两者完全一样：「1 B id +
*   1 B 长度 + 文本值」，因此追加业务参数不需要改编解码，旧记录缺字段时该字段保持默认值。 */
/*! NOTE (命名是历史遗留，2026-09-28)：前 7 项的名字带 `BLE_` 前缀，但 `SVC_PARAM_BLE_SN`
*   自 2026-09-28 起是**设备身份**（MQTT `clientId`/主题里的 `deviceId` + 蓝牙认证应答里的
*   `sn`），不再只是蓝牙参数。名字保留是为了不动 EEPROM 里的字段顺序（记录按 id 编码），
*   语义以本条与 `SVC_Param_IsValidDeviceId()` 为准。 */
typedef enum {
    SVC_PARAM_BLE_SN = 0,   /*!< **设备序列号**（产线串口刷；平台 deviceId 与蓝牙 sn 共用）*/
    SVC_PARAM_BLE_NAME,     /*!< 蓝牙广播名                        */
    SVC_PARAM_BLE_PIN,      /*!< 小程序认证 PIN（凭据，注意纪律）  */
    SVC_PARAM_BLE_UUID,     /*!< Service UUID                      */
    SVC_PARAM_BLE_DEVNAME,  /*!< 设备名                            */
    SVC_PARAM_BLE_MODEL,    /*!< 型号代号                          */
    SVC_PARAM_BLE_BATCH,    /*!< 批次代号                          */
    SVC_PARAM_TRANSINTVL,   /*!< 周期上报间隔，分钟（1~20，业务参数）*/
    SVC_PARAM_COUNT         /*!< 项数（不是合法 id）               */
} SvcParamId;

/* 各字段容量（含结尾 '\0'）。这些是 **schema 常量**：直接决定记录最大长度，
* 改动会影响 A/B 双槽是否仍然放得下（见 app_config.h 的槽偏移）。 */
#define SVC_PARAM_BLE_SN_CAP        (24U)
#define SVC_PARAM_BLE_NAME_CAP      (24U)
#define SVC_PARAM_BLE_PIN_CAP       (12U)
#define SVC_PARAM_BLE_UUID_CAP      (40U)   /* 36 字符 UUID + '\0' + 余量 */
#define SVC_PARAM_BLE_DEVNAME_CAP   (24U)
#define SVC_PARAM_BLE_MODEL_CAP     (16U)
#define SVC_PARAM_BLE_BATCH_CAP     (16U)
/*! `TRANSINTVL`：文本存的是十进制分钟数，最长「20」= 2 字符，留到 3 位有余量。
*  NOTE (为什么按文本存)：与其余字段走同一套编解码（1 B id + 1 B 长度 + 值），
*   不引入新的字段类型；数值范围由命令层（`command_dispatch.c`）与使用方
*   （`modem.c` 读回来时再夹一次）各自校验。 */
#define SVC_PARAM_TRANSINTVL_CAP    (4U)

/*! RAM 影子里每个字段的定长容量。取「最大字段容量」，实现最简单（不做偏移运算），
* 代价是 7×40 = 280 B 的 RAM —— 对 48 KB 的 F103RC 可以接受。 */
#define SVC_PARAM_VALUE_MAX         (SVC_PARAM_BLE_UUID_CAP)

/* 记录格式常量 -------------------------------------------------------------*/
#define SVC_PARAM_MAGIC_0           (0x41U)  /* 'A' */
#define SVC_PARAM_MAGIC_1           (0x54U)  /* 'T' */
#define SVC_PARAM_MAGIC_2           (0x50U)  /* 'P' */
#define SVC_PARAM_MAGIC_3           (0x31U)  /* '1' */
#define SVC_PARAM_FORMAT_VERSION    (1U)
#define SVC_PARAM_HDR_SIZE          (12U)
#define SVC_PARAM_CRC_SIZE          (2U)
#define SVC_PARAM_FLAG_USER_WRITTEN (0x01U)

/*! 记录最大长度：头 + 各字段(1 B id + 1 B len + 值) + CRC。
* 当前字段容量下为 **182 B**，必须小于 app_config.h 里两个槽的间距（256 B），
* 否则第二条记录会压到第一条上。 */
#define SVC_PARAM_RECORD_MAX                                                 \
    (SVC_PARAM_HDR_SIZE + SVC_PARAM_CRC_SIZE +                               \
     (2U + SVC_PARAM_BLE_SN_CAP - 1U) + (2U + SVC_PARAM_BLE_NAME_CAP - 1U) + \
     (2U + SVC_PARAM_BLE_PIN_CAP - 1U) + (2U + SVC_PARAM_BLE_UUID_CAP - 1U) +\
     (2U + SVC_PARAM_BLE_DEVNAME_CAP - 1U) +                                 \
     (2U + SVC_PARAM_BLE_MODEL_CAP - 1U) + (2U + SVC_PARAM_BLE_BATCH_CAP - 1U) + \
     (2U + SVC_PARAM_TRANSINTVL_CAP - 1U))

/*! 参数集合的 RAM 影子。由 App 层持有（本模块不定义全局变量）。 */
typedef struct {
    char     value[SVC_PARAM_COUNT][SVC_PARAM_VALUE_MAX];
    uint16_t config_version;  /*!< 每次成功保存自增                     */
    uint8_t  slot_seq;        /*!< 最近一次保存写入的槽序号             */
    bool     from_media;      /*!< true = 值来自介质上的有效记录        */
    bool     dirty;           /*!< true = 影子比介质新，需要落盘        */
} SvcParam;

/* 记录头解析出的信息（供上层选槽时比较，不必解整条记录） --------------------*/
typedef struct {
    uint8_t  slot_seq;
    uint16_t config_version;
    uint16_t record_len;
    uint8_t  flags;
} SvcParamHeader;

/*--------------------------------------------------------------------------*/
/** @brief 把影子填成编译期默认值（app_config.h 的 APP_CFG_BLE_*）。
* @note 只动内存，不碰介质；上电先调它，之后介质里的有效记录会覆盖它。 */
void SVC_Param_SetDefaults(SvcParam *me);

/** @brief 按 AT 名（小写，大小写不敏感）查参数项。
* @return 0..SVC_PARAM_COUNT-1；找不到返回 -1 */
int16_t SVC_Param_Find(char const *name);

/** @brief 取参数项的 AT 名（小写）。id 非法返回空串。 */
char const *SVC_Param_Name(uint8_t id);

/** @brief 取参数项的容量（含 '\0'）。id 非法返回 0。 */
uint8_t SVC_Param_Cap(uint8_t id);

/** @brief 取参数当前值（永远返回合法 C 字符串，不会返回 NULL）。 */
char const *SVC_Param_Value(SvcParam const *me, uint8_t id);

/** @brief 设置参数值（只改影子，不落盘）。
* @return SVC_OK / SVC_ERR_PARAM（id 非法或空指针）/ SVC_ERR_RANGE（超长或空串） */
svc_status_t SVC_Param_SetValue(SvcParam *me, uint8_t id, char const *text);

/** @brief 把影子编码成一条记录。
* @param me            影子
* @param configVersion 写进记录的 config_version（由上层决定，通常是「当前值 +1」）
* @param slotSeq       写进记录的槽序号（由上层决定）
* @param out           输出缓冲（调用方提供）
* @param cap           输出缓冲容量
* @return 实际写入字节数；0 表示失败（cap 不足）
*/
size_t SVC_Param_Encode(SvcParam const *me, uint16_t configVersion, uint8_t slotSeq,
                        uint8_t *out, size_t cap);

/** @brief 解码一条记录到影子。
* @param me  影子（字段缺省时保留原值；因此调用前应先 SetDefaults()）
* @param in  记录
* @param len 记录长度（必须与记录头里的 rec_len 一致）
* @return SVC_OK / SVC_ERR_PARAM / SVC_ERR_RANGE（长度或格式不符）/ SVC_ERR_CRC
* @note 只写 config_version 与槽序号；`from_media` / `dirty` 由调用方按业务语义设置。
*/
svc_status_t SVC_Param_Decode(SvcParam *me, uint8_t const *in, size_t len);

/** @brief 只解析 12 B 记录头（用于选槽时先看一眼哪份更新，省一次整记录读）。
* @return true = 头合法（magic / 版本 / 长度自洽） */
bool SVC_Param_ParseHeader(uint8_t const *hdr, size_t len, SvcParamHeader *out);

/** @brief 槽序号比较（回绕安全）：a 是否比 b 新。 */
bool SVC_Param_SeqNewer(uint8_t a, uint8_t b);

/** @brief 把「十进制文本」解析成无符号整数（数值型参数共用）。
*
* @param text 待解析文本（如 `"20"`）
* @param out  出参：解析结果（仅成功时写入）
* @return true = 全部字符都是 0~9 且至少 1 位、最多 9 位、无溢出
*
* NOTE (为什么要一个共用的解析)：`TRANSINTVL` 在**命令层**（写入时校验范围）与
*   **使用方**（`modem.c` 每个周期读回来换算）都要把影子里的文本变成数字。
*   两处各写一份解析必然出现「一边收紧、一边放宽」的偏差，所以放在 Service 层共用。
* NOTE (为什么不用 `strtoul`)：它接受前导空白、正负号与 `0x` 前缀，还会设置 `errno`；
*   参数值只可能是纯数字，严格一点可以少一类「看起来解析成功、其实是 0」的坑。 */
bool SVC_Param_TextToU32(char const *text, uint32_t *out);

/** @brief 设备标识的**判据结果**（`SVC_Param_CheckDeviceId()` 的返回值）。
 *
 * NOTE (为什么要分这么多档，2026-09-28 当天订正)：判据上线当天就踩到了——
 *   `AT+SN="…"` 被拒后日志只说 `value rejected`，**看不出到底哪里不合法**
 *   （空串？含非法字符？长度超了？还是没刷？），现场只能靠猜。
 *   所以改成"返回原因 + 给出出错位置"，让拒绝本身可诊断。 */
typedef enum {
    SVC_DEVICE_ID_OK = 0,      /*!< 可用 */
    SVC_DEVICE_ID_EMPTY,       /*!< 空串 */
    SVC_DEVICE_ID_DEFAULT,     /*!< 仍是出厂默认值（= 产线还没刷） */
    SVC_DEVICE_ID_TOO_LONG,    /*!< 长度 ≥ `SVC_PARAM_BLE_SN_CAP` */
    SVC_DEVICE_ID_BAD_CHAR     /*!< 含非法字符（`*badIndex` 给出它在串里的下标） */
} SvcDeviceIdStatus;

/** @brief 该值能否当**设备标识**用（MQTT `clientId` 与全部主题里的 `deviceId`）。
 *
 * @param value    候选值（通常是参数 `sn` 的影子内容）
 * @param badIndex 出参，可为 `NULL`：不合法时写入出错下标（仅 `SVC_DEVICE_ID_BAD_CHAR` 有效）
 * @return `SVC_DEVICE_ID_OK` 才能拿去拼主题；其余是各自的拒绝原因
 *
 * 规则（2026-09-28 起 `sn` 就是设备标识）：
 *   ① 非空；
 *   ② 字符集只允许 `A-Z a-z 0-9` 与 `-` `_` `.`；
 *   ③ 长度 < `SVC_PARAM_BLE_SN_CAP`；
 *   ④ **不等于出厂默认值** `APP_CFG_BLE_SN` —— 那表示「产线还没刷」。
 *
 * NOTE (为什么必须校验，而不是“先让它能跑”)：这个值要嵌进 MQTT 主题。
 *   `/` 是层级分隔符、`+` `#` 是通配符、空格与非 ASCII 各平台支持不一；
 *   而**非法值造成的失败是静默的**——`sensor-product//command` 在 MQTT 里是合法主题，
 *   模组订阅还会回 `+MQTTSUBACK`，只是永远收不到平台的消息（与 `08-改动日志` 里
 *   `#51` 那次踩的是同一类坑）。所以宁可在产线当场拒写、在启动时拒绝上网。
 * NOTE (为什么放在 Service 层)：与 `SVC_Param_TextToU32()` 同理——**同一个判据要被
 *   两处使用**（命令层写入校验、`modem.c` 启动校验），各写一份必然出现
 *   「一边收紧、一边放宽」的偏差。
 * NOTE (要改规则只改这里)：放宽/收紧字符集、换出厂哨兵值，都只动本函数。 */
SvcDeviceIdStatus SVC_Param_CheckDeviceId(char const *value, uint8_t *badIndex);

#endif /* SVC_PARAM_H */
