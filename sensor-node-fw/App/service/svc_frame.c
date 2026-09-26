/*============================================================================
* svc_frame.c —— 服务层：二进制帧组帧与解析实现
*
* 层次定位：Service 层。纯数据处理，无 I/O、无框架依赖。
*===========================================================================*/
#include "svc_frame.h"

/*--------------------------------------------------------------------------*/
/**
* @brief CRC-16/MODBUS（多项式 0xA001，初值 0xFFFF，逐位实现）。
*
* 说明：逐位算法不占查表空间（512 字节），在 72 MHz 下 20 字节载荷约 2 µs 级，
* 对秒级上报完全够用。若以后载荷变长或频率变高，再换成查表法。
*/
uint16_t SVC_Frame_Crc16(uint8_t const *data, size_t len) {
    uint16_t crc = 0xFFFFU;
    size_t i;
    uint8_t b;

    if (data == (uint8_t const *)0) {
        return 0U;
    }

    for (i = 0U; i < len; ++i) {
        crc ^= (uint16_t)data[i];
        for (b = 0U; b < 8U; ++b) {
            if ((crc & 1U) != 0U) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001U);
            }
            else {
                crc = (uint16_t)(crc >> 1);
            }
        }
    }

    return crc;
}

/*--------------------------------------------------------------------------*/
svc_status_t SVC_Frame_Pack(uint8_t cmd, uint8_t const *payload, size_t len,
                            uint8_t *out, size_t cap, size_t *outLen) {
    size_t total;
    size_t i;
    uint16_t crc;

    if ((out == (uint8_t *)0) || (outLen == (size_t *)0)) {
        return SVC_ERR_PARAM;
    }
    if ((len > SVC_FRAME_MAX_PAYLOAD) || ((len > 0U) && (payload == (uint8_t const *)0))) {
        return SVC_ERR_RANGE;
    }

    total = SVC_FRAME_HDR_SIZE + len + SVC_FRAME_CRC_SIZE;
    if (cap < total) {
        return SVC_ERR_NOMEM; /* 不做部分写入 */
    }

    out[0] = SVC_FRAME_SOF0;
    out[1] = SVC_FRAME_SOF1;
    out[2] = (uint8_t)len;
    out[3] = cmd;
    for (i = 0U; i < len; ++i) {
        out[SVC_FRAME_HDR_SIZE + i] = payload[i];
    }

    /* CRC 覆盖 LEN、CMD、PAYLOAD（从索引 2 开始，长度 2 + len） */
    crc = SVC_Frame_Crc16(&out[2], 2U + len);
    out[total - 2U] = (uint8_t)(crc & 0xFFU);        /* 小端：低字节在前 */
    out[total - 1U] = (uint8_t)((crc >> 8) & 0xFFU);

    *outLen = total;
    return SVC_OK;
}

/*--------------------------------------------------------------------------*/
svc_status_t SVC_Frame_Parse(uint8_t const *in, size_t len,
                             uint8_t *cmdOut, uint8_t *payloadOut,
                             size_t cap, size_t *payloadLen) {
    size_t plen;
    size_t total;
    uint16_t crc;
    uint16_t rx;
    size_t i;

    if ((in == (uint8_t const *)0) || (cmdOut == (uint8_t *)0) ||
        (payloadLen == (size_t *)0)) {
        return SVC_ERR_PARAM;
    }
    if (len < (SVC_FRAME_HDR_SIZE + SVC_FRAME_CRC_SIZE)) {
        return SVC_ERR_RANGE;
    }
    if ((in[0] != SVC_FRAME_SOF0) || (in[1] != SVC_FRAME_SOF1)) {
        return SVC_ERR_RANGE; /* 帧同步失败 */
    }

    plen = (size_t)in[2];
    if (plen > SVC_FRAME_MAX_PAYLOAD) {
        return SVC_ERR_RANGE;
    }

    total = SVC_FRAME_HDR_SIZE + plen + SVC_FRAME_CRC_SIZE;
    if (len != total) {
        return SVC_ERR_RANGE; /* 严格模式：长度必须完全一致 */
    }

    crc = SVC_Frame_Crc16(&in[2], 2U + plen);
    rx = (uint16_t)((uint16_t)in[total - 2U] | ((uint16_t)in[total - 1U] << 8));
    if (crc != rx) {
        return SVC_ERR_CRC;
    }

    if ((payloadOut != (uint8_t *)0) && (plen > 0U)) {
        if (cap < plen) {
            return SVC_ERR_NOMEM;
        }
        for (i = 0U; i < plen; ++i) {
            payloadOut[i] = in[SVC_FRAME_HDR_SIZE + i];
        }
    }

    *cmdOut = in[3];
    *payloadLen = plen;
    return SVC_OK;
}
