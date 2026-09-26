/*============================================================================
* svc_frame.h —— 服务层：二进制帧组帧与解析
*
* 层次定位：App(应用层) → **Service(服务层)** → Driver → BSP → HAL/CMSIS
*
* 帧格式（对应 docs_sensor/06-协议格式.md#规划中的二进制帧 的落地版本）：
*
*   +--------+--------+------+------+-----------+---------+
*   | SOF0   | SOF1   | LEN  | CMD  | PAYLOAD   | CRC16   |
*   | 0xAA   | 0x55   | 1 B  | 1 B  | LEN 字节  | 2 B 小端 |
*   +--------+--------+------+------+-----------+---------+
*
* 约定：
*   - LEN 只表示 PAYLOAD 长度，不含帧头与 CRC
*   - CRC 算法为 CRC-16/MODBUS（多项式 0xA001，初值 0xFFFF），
*     覆盖范围是 LEN、CMD、PAYLOAD 三段（不含 SOF，也不含 CRC 自身）
*   - 缓冲由调用方提供：入参 out + cap，出参 *outLen；cap 不足返回
*     SVC_ERR_NOMEM，**不做部分写入**
*   - 本层不得 #include "qpc.h"，也不做 I/O 与重传等待
*===========================================================================*/
#ifndef SVC_FRAME_H
#define SVC_FRAME_H

#include <stdint.h>
#include <stddef.h>

/* Service 层状态码（本层专用，不跨层复用；见 07-接口约定#状态码体系） */
typedef enum {
    SVC_OK = 0,      /* 成功                         */
    SVC_ERR_PARAM,   /* 入参非法（空指针等）         */
    SVC_ERR_RANGE,   /* 长度 / 数值越界              */
    SVC_ERR_CRC,     /* 校验失败                     */
    SVC_ERR_STATE,   /* 下层设备不可用等状态问题     */
    SVC_ERR_NOMEM    /* 调用方缓冲不足               */
} svc_status_t;

#define SVC_FRAME_SOF0        (0xAAU)
#define SVC_FRAME_SOF1        (0x55U)
#define SVC_FRAME_HDR_SIZE    (4U)  /* SOF0 + SOF1 + LEN + CMD */
#define SVC_FRAME_CRC_SIZE    (2U)
#define SVC_FRAME_MAX_PAYLOAD (32U)
#define SVC_FRAME_MAX_SIZE    (SVC_FRAME_HDR_SIZE + SVC_FRAME_MAX_PAYLOAD + SVC_FRAME_CRC_SIZE)

/*! 计算 CRC-16/MODBUS。 */
uint16_t SVC_Frame_Crc16(uint8_t const *data, size_t len);

/*! 组一帧。
* @param cmd      命令码
* @param payload  载荷，可为 NULL（当 len 为 0）
* @param len      载荷长度，上限 SVC_FRAME_MAX_PAYLOAD
* @param out      输出缓冲（调用方提供）
* @param cap      输出缓冲容量（字节）
* @param outLen   出参：实际写入的帧长度
* @return SVC_OK / SVC_ERR_PARAM / SVC_ERR_RANGE / SVC_ERR_NOMEM
*/
svc_status_t SVC_Frame_Pack(uint8_t cmd, uint8_t const *payload, size_t len,
                            uint8_t *out, size_t cap, size_t *outLen);

/*! 解析一帧（严格模式：长度必须完全匹配）。
* @param in          输入缓冲
* @param len         输入缓冲中可用的字节数
* @param cmdOut      出参：命令码
* @param payloadOut  出参：载荷输出缓冲（可为 NULL，此时只校验不取载荷）
* @param cap         payloadOut 的容量
* @param payloadLen  出参：载荷长度
* @return SVC_OK / SVC_ERR_PARAM / SVC_ERR_RANGE / SVC_ERR_CRC / SVC_ERR_NOMEM
*/
svc_status_t SVC_Frame_Parse(uint8_t const *in, size_t len,
                             uint8_t *cmdOut, uint8_t *payloadOut,
                             size_t cap, size_t *payloadLen);

#endif /* SVC_FRAME_H */
