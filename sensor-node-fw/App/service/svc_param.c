/*============================================================================
* svc_param.c —— 服务层：设备参数记录编解码实现
*
* 层次定位：Service 层。纯数据处理：不碰总线、不碰 HAL、不含 QP。
* 记录格式与约定见 svc_param.h 的文件头。
*===========================================================================*/
#include "svc_param.h"

#include <string.h>

/*--------------------------------------------------------------------------*/
/* 参数表：AT 名 / 容量 / 编译期默认值                                     */
/*--------------------------------------------------------------------------*/
typedef struct {
    char const *at_name;   /*!< 指令里用的名字（小写，比较时大小写不敏感） */
    uint8_t     cap;       /*!< 容量，含结尾 '\0'                          */
    char const *fallback;  /*!< 编译期默认值（app_config.h 的 APP_CFG_BLE_*） */
} ParamDesc;

/* NOTE (增删字段)：只在表尾追加，不要往中间插——记录里的字段顺序由 id 决定，
* 中间插值会让已固化在介质上的旧记录串位（fmt 版本号只在语义变化时才动）。 */
static ParamDesc const l_desc[SVC_PARAM_COUNT] = {
    { "sn",      SVC_PARAM_BLE_SN_CAP,      APP_CFG_BLE_SN           },
    { "name",    SVC_PARAM_BLE_NAME_CAP,    APP_CFG_BLE_NAME         },
    { "pin",     SVC_PARAM_BLE_PIN_CAP,     APP_CFG_BLE_PIN          },
    { "uuid",    SVC_PARAM_BLE_UUID_CAP,    APP_CFG_BLE_SERVICE_UUID },
    { "devname", SVC_PARAM_BLE_DEVNAME_CAP, APP_CFG_BLE_DEVICE_NAME  },
    { "model",   SVC_PARAM_BLE_MODEL_CAP,   APP_CFG_BLE_MODEL_CODE   },
    { "batch",   SVC_PARAM_BLE_BATCH_CAP,   APP_CFG_BLE_BATCH_CODE   },
    /* 业务参数：周期上报间隔（分钟）。AT 形态是 `AT+TRANSINTVL?` / `AT+TRANSINTVL=<1..20>`，
     * 由命令层按 app_config.h 的 APP_CFG_MODEM_TRANS_INTERVAL_MIN/MAX 做范围校验。 */
    { "transintvl", SVC_PARAM_TRANSINTVL_CAP, APP_CFG_MODEM_TRANS_INTERVAL_DEFAULT_TEXT }
};

/* 编译期自检（负数组长度 = 编译报错）：
*  ① 字段容量不能超过影子的定长；
*  ② 最大记录必须放得进一个槽的间距（否则第二份记录会压到第一份上）；
*  ③ 最后一个槽的记录必须落在器件容量之内。 */
typedef char svcParamCapCheck[(SVC_PARAM_BLE_UUID_CAP <= SVC_PARAM_VALUE_MAX) ? 1 : -1];
typedef char svcParamSlotGapCheck[
    ((uint32_t)APP_CFG_STORE_SLOT_B_ADDR - (uint32_t)APP_CFG_STORE_SLOT_A_ADDR
     >= (uint32_t)SVC_PARAM_RECORD_MAX) ? 1 : -1];
typedef char svcParamCapacityCheck[
    (((uint32_t)APP_CFG_STORE_SLOT_B_ADDR + (uint32_t)SVC_PARAM_RECORD_MAX)
     <= (uint32_t)APP_CFG_STORE_CAPACITY) ? 1 : -1];

/*--------------------------------------------------------------------------*/
/* 内部工具                                                                  */
/*--------------------------------------------------------------------------*/
/** @brief 大小写无关的短字符串比较（ASCII；不用 POSIX 的 strcasecmp，
* 它在 ARM Compiler 的标准库里不保证可见）。 */
static bool nameEquals(char const *a, char const *b) {
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
/** @brief 带长度上限的字符串拷贝（含结尾 '\0'）。 */
static void copyCapped(char *dst, uint8_t cap, char const *src) {
    size_t n = strlen(src);

    if (cap == 0U) {
        return;
    }
    if (n >= (size_t)cap) {
        n = (size_t)cap - 1U;
    }
    (void)memcpy(dst, src, n);
    dst[n] = '\0';
}

/*--------------------------------------------------------------------------*/
/* 默认值与查询                                                              */
/*--------------------------------------------------------------------------*/
void SVC_Param_SetDefaults(SvcParam *me) {
    uint8_t i;

    if (me == (SvcParam *)0) {
        return;
    }
    for (i = 0U; i < (uint8_t)SVC_PARAM_COUNT; ++i) {
        copyCapped(me->value[i], SVC_PARAM_VALUE_MAX, l_desc[i].fallback);
    }
    me->config_version = 0U;
    me->slot_seq = 0U;
    me->from_media = false;
    me->dirty = false;
}

/*..........................................................................*/
int16_t SVC_Param_Find(char const *name) {
    uint8_t i;

    if (name == (char const *)0) {
        return -1;
    }
    for (i = 0U; i < (uint8_t)SVC_PARAM_COUNT; ++i) {
        if (nameEquals(name, l_desc[i].at_name)) {
            return (int16_t)i;
        }
    }
    return -1;
}

/*..........................................................................*/
char const *SVC_Param_Name(uint8_t id) {
    return (id < (uint8_t)SVC_PARAM_COUNT) ? l_desc[id].at_name : "";
}

/*..........................................................................*/
uint8_t SVC_Param_Cap(uint8_t id) {
    return (id < (uint8_t)SVC_PARAM_COUNT) ? l_desc[id].cap : 0U;
}

/*..........................................................................*/
char const *SVC_Param_Value(SvcParam const *me, uint8_t id) {
    if ((me == (SvcParam const *)0) || (id >= (uint8_t)SVC_PARAM_COUNT)) {
        return "";
    }
    return me->value[id];
}

/*..........................................................................*/
svc_status_t SVC_Param_SetValue(SvcParam *me, uint8_t id, char const *text) {
    size_t n;

    if ((me == (SvcParam *)0) || (text == (char const *)0) ||
        (id >= (uint8_t)SVC_PARAM_COUNT)) {
        return SVC_ERR_PARAM;
    }

    n = strlen(text);
    if ((n == 0U) || (n >= (size_t)l_desc[id].cap) ||
        (n >= (size_t)SVC_PARAM_VALUE_MAX)) {
        return SVC_ERR_RANGE;   /* 空串与超长都拒绝，避免写出「看起来成功」的假值 */
    }

    (void)memcpy(me->value[id], text, n);
    me->value[id][n] = '\0';
    return SVC_OK;
}

/*--------------------------------------------------------------------------*/
/* 编码                                                                      */
/*--------------------------------------------------------------------------*/
size_t SVC_Param_Encode(SvcParam const *me, uint16_t configVersion, uint8_t slotSeq,
                        uint8_t *out, size_t cap) {
    size_t p;
    size_t total;
    uint16_t crc;
    uint8_t i;

    if ((me == (SvcParam const *)0) || (out == (uint8_t *)0)) {
        return 0U;
    }
    /* NOTE (为什么按最坏长度卡)：记录是变长的，但这里要求缓冲能放下**最坏情况**，
    * 这样任何字段都不会被截断；否则会出现「保存成功、值却少了一半」。 */
    if (cap < (size_t)SVC_PARAM_RECORD_MAX) {
        return 0U;
    }

    p = (size_t)SVC_PARAM_HDR_SIZE;
    for (i = 0U; i < (uint8_t)SVC_PARAM_COUNT; ++i) {
        size_t n = strlen(me->value[i]);
        if (n >= (size_t)l_desc[i].cap) {
            n = (size_t)l_desc[i].cap - 1U;
        }
        out[p] = i;
        out[p + 1U] = (uint8_t)n;
        p += 2U;
        (void)memcpy(&out[p], me->value[i], n);
        p += n;
    }
    total = p + (size_t)SVC_PARAM_CRC_SIZE;

    out[0] = SVC_PARAM_MAGIC_0;
    out[1] = SVC_PARAM_MAGIC_1;
    out[2] = SVC_PARAM_MAGIC_2;
    out[3] = SVC_PARAM_MAGIC_3;
    out[4] = SVC_PARAM_FORMAT_VERSION;
    out[5] = slotSeq;
    out[6] = (uint8_t)(configVersion & 0xFFU);
    out[7] = (uint8_t)((configVersion >> 8) & 0xFFU);
    /* flags bit0 = 「不是纯编译期默认值」：用户改过或从介质读回来过，都算。 */
    out[8] = ((me->dirty || me->from_media) ? SVC_PARAM_FLAG_USER_WRITTEN : 0U);
    out[9] = (uint8_t)SVC_PARAM_COUNT;
    out[10] = (uint8_t)(total & 0xFFU);
    out[11] = (uint8_t)((total >> 8) & 0xFFU);

    crc = SVC_Frame_Crc16(out, total - (size_t)SVC_PARAM_CRC_SIZE);
    out[total - 2U] = (uint8_t)(crc & 0xFFU);
    out[total - 1U] = (uint8_t)((crc >> 8) & 0xFFU);

    return total;
}

/*--------------------------------------------------------------------------*/
/* 解码                                                                      */
/*--------------------------------------------------------------------------*/
svc_status_t SVC_Param_Decode(SvcParam *me, uint8_t const *in, size_t len) {
    size_t p;
    size_t end;
    size_t recLen;
    uint16_t crc;
    uint16_t rx;
    uint8_t fc;

    if ((me == (SvcParam *)0) || (in == (uint8_t const *)0)) {
        return SVC_ERR_PARAM;
    }
    if (len < ((size_t)SVC_PARAM_HDR_SIZE + (size_t)SVC_PARAM_CRC_SIZE)) {
        return SVC_ERR_RANGE;
    }
    if ((in[0] != SVC_PARAM_MAGIC_0) || (in[1] != SVC_PARAM_MAGIC_1) ||
        (in[2] != SVC_PARAM_MAGIC_2) || (in[3] != SVC_PARAM_MAGIC_3)) {
        return SVC_ERR_RANGE;   /* 不是本模块写的记录（空白介质读出来就是全 0xFF 或全 0） */
    }
    if (in[4] != SVC_PARAM_FORMAT_VERSION) {
        return SVC_ERR_RANGE;   /* 格式版本不认识：宁可回落到默认值，也不要猜 */
    }

    recLen = (size_t)((uint16_t)in[10] | ((uint16_t)in[11] << 8));
    if (recLen != len) {
        return SVC_ERR_RANGE;   /* 长度不自洽：多半是写到一半掉电 */
    }

    crc = SVC_Frame_Crc16(in, len - (size_t)SVC_PARAM_CRC_SIZE);
    rx = (uint16_t)((uint16_t)in[len - 2U] | ((uint16_t)in[len - 1U] << 8));
    if (crc != rx) {
        return SVC_ERR_CRC;
    }

    end = len - (size_t)SVC_PARAM_CRC_SIZE;
    fc = in[9];
    p = (size_t)SVC_PARAM_HDR_SIZE;
    while ((fc > 0U) && ((p + 2U) <= end)) {
        uint8_t const id = in[p];
        size_t n = (size_t)in[p + 1U];
        p += 2U;
        if (n > (end - p)) {
            return SVC_ERR_RANGE;
        }
        if (id < (uint8_t)SVC_PARAM_COUNT) {
            /* 双保险：既不超过该字段的 schema 容量，也不超过影子数组本身 */
            size_t lim = (size_t)l_desc[id].cap - 1U;
            if (lim > ((size_t)SVC_PARAM_VALUE_MAX - 1U)) {
                lim = (size_t)SVC_PARAM_VALUE_MAX - 1U;
            }
            if (n > lim) {
                n = lim;
            }
            (void)memcpy(me->value[id], &in[p], n);
            me->value[id][n] = '\0';
        }
        /* id 超出当前 schema（旧固件读新记录）：跳过该字段，保留默认值 */
        p += (size_t)in[p - 1U];
        --fc;
    }

    me->config_version = (uint16_t)((uint16_t)in[6] | ((uint16_t)in[7] << 8));
    me->slot_seq = in[5];
    return SVC_OK;
}

/*--------------------------------------------------------------------------*/
/* 记录头                                                                    */
/*--------------------------------------------------------------------------*/
bool SVC_Param_ParseHeader(uint8_t const *hdr, size_t len, SvcParamHeader *out) {
    uint16_t recLen;

    if ((hdr == (uint8_t const *)0) || (out == (SvcParamHeader *)0) ||
        (len < (size_t)SVC_PARAM_HDR_SIZE)) {
        return false;
    }
    if ((hdr[0] != SVC_PARAM_MAGIC_0) || (hdr[1] != SVC_PARAM_MAGIC_1) ||
        (hdr[2] != SVC_PARAM_MAGIC_2) || (hdr[3] != SVC_PARAM_MAGIC_3)) {
        return false;
    }
    if (hdr[4] != SVC_PARAM_FORMAT_VERSION) {
        return false;
    }

    recLen = (uint16_t)((uint16_t)hdr[10] | ((uint16_t)hdr[11] << 8));
    if ((recLen < (uint16_t)(SVC_PARAM_HDR_SIZE + SVC_PARAM_CRC_SIZE)) ||
        (recLen > (uint16_t)SVC_PARAM_RECORD_MAX)) {
        return false;
    }

    out->slot_seq = hdr[5];
    out->config_version = (uint16_t)((uint16_t)hdr[6] | ((uint16_t)hdr[7] << 8));
    out->record_len = recLen;
    out->flags = hdr[8];
    return true;
}

/*..........................................................................*/
bool SVC_Param_SeqNewer(uint8_t a, uint8_t b) {
    /* 槽序号是 8 位自然回绕的计数器：用有符号差值比较，跨 255→0 时仍然正确
    * （例如 a=1、b=254 判定 a 更新）。 */
    return ((int8_t)(a - b) > 0);
}

/*..........................................................................*/
bool SVC_Param_TextToU32(char const *text, uint32_t *out) {
    uint32_t value = 0U;
    uint8_t digits = 0U;

    if ((text == (char const *)0) || (out == (uint32_t *)0) || (text[0] == '\0')) {
        return false;
    }

    while (*text != '\0') {
        /* 只收 0~9：前导空白、正负号、0x 前缀、小数点一律判非法（参数值不该长这样） */
        if ((*text < '0') || (*text > '9') || (digits >= 9U)) {
            return false;
        }
        value = (value * 10U) + (uint32_t)(*text - '0');
        ++digits;
        ++text;
    }

    *out = value;
    return true;
}

/*..........................................................................*/
SvcDeviceIdStatus SVC_Param_CheckDeviceId(char const *value, uint8_t *badIndex) {
    uint8_t index = 0U;

    if (badIndex != (uint8_t *)0) {
        *badIndex = 0U;
    }
    if ((value == (char const *)0) || (value[0] == '\0')) {
        return SVC_DEVICE_ID_EMPTY;   /* 拼出来是 `sensor-product//command` 这种没人发的主题 */
    }
    /* 出厂默认值 = 产线还没刷。NOTE：换哨兵值时改这一行（或改 app_config.h 的默认值）。
     * NOTE (为什么只在有存储时判，2026-09-28)：`APP_CFG_STORE_ENABLE = 0` 时影子永远只有
     *   编译期默认值，而**没有任何途径把它改成别的**（写只改 RAM、掉电即丢）。
     *   若照样拒绝，那个构建的 4G 会永远起不来——所以这种构建下不把默认值当"未配置"。 */
#if (APP_CFG_STORE_ENABLE != 0)
    if (strcmp(value, APP_CFG_BLE_SN) == 0) {
        return SVC_DEVICE_ID_DEFAULT;
    }
#endif
    if (strlen(value) >= (size_t)SVC_PARAM_BLE_SN_CAP) {
        return SVC_DEVICE_ID_TOO_LONG;   /* SetValue 本已按容量拒，这里是兜底 */
    }
    while (*value != '\0') {
        char const c = *value;
        bool const ok = ((c >= 'A') && (c <= 'Z')) || ((c >= 'a') && (c <= 'z')) ||
                        ((c >= '0') && (c <= '9')) ||
                        (c == '-') || (c == '_') || (c == '.');
        if (!ok) {
            if (badIndex != (uint8_t *)0) {
                *badIndex = index;
            }
            return SVC_DEVICE_ID_BAD_CHAR;
        }
        ++value;
        ++index;
    }
    return SVC_DEVICE_ID_OK;
}
