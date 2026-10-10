/*============================================================================
* drv_scl3300.c —— 器件驱动层：SCL3300-D01 三轴倾角仪（SPI，非阻塞）
*
* 层次定位：App → Service → **Driver(器件驱动层)** → BSP → HAL/CMSIS
* 目标器件：Murata SCL3300-D01；手册 Doc.No. 4921 Rev. 4。
* 片选：PA4（经 BSP_SPI_SENSOR 槽登记）；总线：共享 SPI1，Mode 0，APB2/32。
* 所属内核：QP/C 6.9.1 QV（协作式单栈）——本文件**任何函数都不得阻塞**。
*
* 手册要点（本实现全部依据这几条，改动前请回查手册原文）：
*   5.1.2 错帧协议：本次帧的 MISO 回的是**上一条命令**的结果，因此**每个寄存器
*         都要发两帧**：第 1 帧提交命令（响应丢弃），第 2 帧取数据。
*         复位后的第一个响应未定义，必须丢弃。
*         相邻请求之间 CSB 为高 ≥10 µs，否则「output data will be corrupted」。
*         Mode 0（CPOL=0 / CPHA=0），MSB first。
*   5.1.3 帧结构：OP[31:26]（bit5=RW，bit4:0=ADDR）+ RS[25:24] + D[23:8] + CRC[7:0]。
*         写命令的动作码放在 D 字段（例如切 Mode 3 = B4 00 02 25）。
*   5.1.5 RS 位：00=启动中、01=正常、10=保留、11=错误。
*   5.2   CRC：多项式 0x1D、初值 0xFF、结果取反，覆盖高 24 位。官方给出了
*         参考 C 代码（Figure 15），下面 SCL_Crc8/SCL_CalcCrc 照抄其算法。
*   4.2   启动流程：SW Reset → 等 3 ms → 设测量模式 → 写 ANG_CTRL → 丢首帧 → 读 STATUS。
*   4.1   仅在 RS 指示错误时才需要读 STATUS，因此正常采样不附带读 STATUS。
*
* NOTE (非阻塞设计)：一次采样读 7 个量（X/Y/Z 角度 + X/Y/Z 加速度 + 温度），
*   每个量两帧，共 14 帧。全部等待都用「时间戳 + 下一次 Process()」实现：
*   发起一笔 SPI 事务后立刻返回，下一拍再问 BSP 要结果。绝不 busy-wait——
*   这是 AGENTS.md 5.2 节的硬性要求。
*===========================================================================*/
#include "drv_scl3300.h"
#include "app_config.h"
#include "bsp.h"

#include <string.h>

/*--------------------------------------------------------------- 手册常量 */
/*! 命令帧的 ADDR 字段（OP[4:0]），取自手册表 15/18 的寄存器地址。 */
#define SCL_ADDR_ACC_X      (0x01U)
#define SCL_ADDR_ACC_Y      (0x02U)
#define SCL_ADDR_ACC_Z      (0x03U)
#define SCL_ADDR_TEMP       (0x05U)
#define SCL_ADDR_STATUS     (0x06U)
#define SCL_ADDR_ANG_X      (0x09U)
#define SCL_ADDR_ANG_Y      (0x0AU)
#define SCL_ADDR_ANG_Z      (0x0BU)
#define SCL_ADDR_ANG_CTRL   (0x0CU)
#define SCL_ADDR_CMD        (0x0DU)
#define SCL_ADDR_WHOAMI     (0x10U)

/*! 命令帧的 D 字段（写命令的动作码，手册表 15）。 */
#define SCL_DATA_SW_RESET   (0x2098U)
#define SCL_DATA_MODE1      (0x001FU)
#define SCL_DATA_MODE2      (0x0102U)
#define SCL_DATA_MODE3      (0x0225U)
#define SCL_DATA_MODE4      (0x0338U)
#define SCL_DATA_ANG_EN     (0x1F6FU)

/*! WHOAMI 的期望返回值（手册 6.7：Returned value is C1h）。 */
#define SCL_WHOAMI_VALUE    (0x00C1U)

/*! RS 位取值（手册 5.1.5 表 16）：01 = 正常运行、无标志。 */
#define SCL_RS_OK           (0x01U)

/*! STATUS 位（手册 6.3 表 27）。 */
#define SCL_STATUS_SAT      (1U << 6)   /* 信号路径饱和：本帧数据不可用 */
#define SCL_STATUS_INVALID  (0xFFFFU)   /* 还没读到过 STATUS */

/*! 帧长度：4 字节（32 位）。 */
#define SCL_FRAME_LEN       (4U)

/*! 采样点数：X/Y/Z 角度 + X/Y/Z 加速度 + 温度。 */
#define SCL_POINT_COUNT     (7U)

/*! `point` 的哨兵值：表示「当前没有在等某个采样点的响应」。 */
#define SCL_POINT_NONE      (0xFFU)

/*--------------------------------------------------------------- 命令表 */
/*! 以「地址 + 数据 + 读/写」描述一条命令；帧的 CRC 在运行期算。 */
typedef struct {
    uint8_t  addr;
    uint16_t data;
    bool     write;
} SclCmd;

static SclCmd const kReadAccX   = { SCL_ADDR_ACC_X,    0x0000U,           false };
static SclCmd const kReadAccY   = { SCL_ADDR_ACC_Y,    0x0000U,           false };
static SclCmd const kReadAccZ   = { SCL_ADDR_ACC_Z,    0x0000U,           false };
static SclCmd const kReadTemp   = { SCL_ADDR_TEMP,     0x0000U,           false };
static SclCmd const kReadStatus = { SCL_ADDR_STATUS,   0x0000U,           false };
static SclCmd const kReadAngX   = { SCL_ADDR_ANG_X,    0x0000U,           false };
static SclCmd const kReadAngY   = { SCL_ADDR_ANG_Y,    0x0000U,           false };
static SclCmd const kReadAngZ   = { SCL_ADDR_ANG_Z,    0x0000U,           false };
static SclCmd const kReadWho    = { SCL_ADDR_WHOAMI,   0x0000U,           false };
static SclCmd const kSwReset    = { SCL_ADDR_CMD,      SCL_DATA_SW_RESET, true  };
static SclCmd const kAngEnable  = { SCL_ADDR_ANG_CTRL, SCL_DATA_ANG_EN,   true  };
static SclCmd const kMode1      = { SCL_ADDR_CMD,      SCL_DATA_MODE1,    true  };
static SclCmd const kMode2      = { SCL_ADDR_CMD,      SCL_DATA_MODE2,    true  };
static SclCmd const kMode3      = { SCL_ADDR_CMD,      SCL_DATA_MODE3,    true  };
static SclCmd const kMode4      = { SCL_ADDR_CMD,      SCL_DATA_MODE4,    true  };

/*! 采样点顺序；`SCL_PointCmd()` 与 `SCL_StorePoint()` 必须共用同一套索引。 */
typedef enum {
    SCL_IDX_ANG_X = 0,
    SCL_IDX_ANG_Y,
    SCL_IDX_ANG_Z,
    SCL_IDX_ACC_X,
    SCL_IDX_ACC_Y,
    SCL_IDX_ACC_Z,
    SCL_IDX_TEMP
} SclPointIndex;

/*--------------------------------------------------------------- 内部状态 */
typedef enum {
    SCL_INIT_RESET = 0,   /* 发 SW Reset                     */
    SCL_INIT_AFTER_RESET, /* 等手册 4.2 的 3 ms              */
    SCL_INIT_SET_MODE,    /* 写测量模式                      */
    SCL_INIT_ANG_EN,      /* 写 ANG_CTRL 使能角度            */
    SCL_INIT_WHOAMI,      /* 读 WHOAMI（判 C1h）             */
    SCL_INIT_DISCARD,     /* 丢弃复位后的第一个响应          */
    SCL_INIT_STATUS,      /* 读 STATUS（读即清）             */
    SCL_READY
} SclInitState;

typedef enum {
    SCL_JOB_NONE = 0,
    SCL_JOB_RUNNING,
    SCL_JOB_DONE,
    SCL_JOB_FAILED
} SclJobState;

typedef struct {
    SclInitState init;
    SclJobState  job;
    drv_status_t last_error;

    bool     frame_pending;   /* true = 已发起一笔事务，等 BSP 完成 */
    uint8_t  point;           /* 当前采样点索引；SCL_POINT_NONE = 无 */
    bool     point_first;     /* true = 该点的第 1 帧（提交命令）    */

    uint32_t last_bus_ms;     /* 上一笔事务结束的时刻（帧间隔基准）  */
    uint32_t reset_ms;        /* SW Reset 发出的时刻（等 3 ms）      */
    uint32_t job_ms;          /* 本次采样作业的起始时刻              */

    uint8_t  tx[SCL_FRAME_LEN];
    uint8_t  rx[SCL_FRAME_LEN];

    uint16_t status;
    bool     status_valid;

    DrvSensorSample sample;
} SclDriver;

static SclDriver l_scl;

/*--------------------------------------------------------------- CRC（手册 Figure 15） */
/** @brief CRC 的逐位推进；与手册 Figure 15 的 CRC8() 完全一致。 */
static uint8_t SCL_Crc8(uint8_t bit_value, uint8_t crc) {
    uint8_t temp = (uint8_t)(crc & 0x80U);
    if (bit_value == 0x01U) {
        temp ^= 0x80U;
    }
    crc = (uint8_t)(crc << 1);
    if (temp > 0U) {
        crc ^= 0x1DU;
    }
    return crc;
}

/**
* @brief 计算 32 位 dword 高 24 位的 CRC（低 8 位是 CRC 字段本身，不参与）。
* @param data32 待计算的值；只有 [31:8] 参与计算
* @return CRC 字节（结果已取反）
*/
static uint8_t SCL_CalcCrc(uint32_t data32) {
    uint8_t crc = 0xFFU;
    uint8_t i;
    for (i = 31U; i > 7U; --i) {
        crc = SCL_Crc8((uint8_t)((data32 >> i) & 0x01U), crc);
    }
    return (uint8_t)(~crc);
}

/**
* @brief 按手册 5.1.3 组装一条完整的 32 位命令帧。
* @param cmd 命令描述
* @param out 输出：4 字节，MSB first 的发送顺序
*/
static void SCL_BuildFrame(SclCmd const *cmd, uint8_t out[SCL_FRAME_LEN]) {
    uint32_t op = (uint32_t)((cmd->write ? 0x20U : 0x00U) | (cmd->addr & 0x1FU));
    uint32_t val = ((op << 26) | ((uint32_t)cmd->data & 0xFFFFU)) & 0xFFFFFF00UL;
    uint32_t frame = val | (uint32_t)SCL_CalcCrc(val);
    out[0] = (uint8_t)(frame >> 24);
    out[1] = (uint8_t)(frame >> 16);
    out[2] = (uint8_t)(frame >> 8);
    out[3] = (uint8_t)(frame);
}

/*--------------------------------------------------------------- 响应解析 */
/** @brief 取 16 位数据字段 D[23:8]（小端：b1 + (b2 << 8)）。 */
static uint16_t SCL_RxData(void) {
    return (uint16_t)((uint16_t)l_scl.rx[2] | ((uint16_t)l_scl.rx[1] << 8));
}

/** @brief 取 MISO 的 RS 位（首字节低 2 位）：01 = 正常，11 = 错误。 */
static uint8_t SCL_RxRs(void) {
    return (uint8_t)(l_scl.rx[0] & 0x03U);
}

/** @brief 校验响应帧的 CRC（拿收到的整 32 位重算，与收到的 CRC 字节比）。 */
static bool SCL_RxCrcOk(void) {
    uint32_t got = ((uint32_t)l_scl.rx[0] << 24) | ((uint32_t)l_scl.rx[1] << 16) |
                   ((uint32_t)l_scl.rx[2] << 8) | (uint32_t)l_scl.rx[3];
    return SCL_CalcCrc(got & 0xFFFFFF00UL) == l_scl.rx[3];
}

/*--------------------------------------------------------------- 帧调度 */
/** @brief 帧间隔是否已满（CSB 高 ≥ 手册要求的 10 µs，这里按配置留足裕量）。 */
static bool SCL_GapElapsed(void) {
    return (uint32_t)(BSP_uptimeMs() - l_scl.last_bus_ms) >=
           (uint32_t)APP_CFG_SCL3300_FRAME_GAP_MS;
}

/**
* @brief 发起一帧（一次 4 字节 SPI 事务），随后立刻返回。
* @param cmd 要发送的命令
* @param point 该响应归属的采样点；SCL_POINT_NONE = 与采样无关（初始化用）
* @param first true = 该点的第 1 帧（提交命令）；false = 第 2 帧（取数据）
* @return true = 已发起
*/
static bool SCL_Submit(SclCmd const *cmd, uint8_t point, bool first) {
    bsp_status_t st;
    if (BSP_spiIsBusy()) {
        return false;
    }
    SCL_BuildFrame(cmd, l_scl.tx);
    (void)memset(l_scl.rx, 0, sizeof(l_scl.rx));
    st = BSP_spiTransferStart(BSP_SPI_SENSOR, l_scl.tx, l_scl.rx, (uint16_t)SCL_FRAME_LEN);
    if (st != BSP_OK) {
        return false;
    }
    l_scl.point = point;
    l_scl.point_first = first;
    l_scl.frame_pending = true;
    return true;
}

/** @brief 发一帧全 0，用于丢弃复位后的第一个未定义响应。 */
static bool SCL_SubmitDiscard(void) {
    static uint8_t const zeros[SCL_FRAME_LEN] = { 0U, 0U, 0U, 0U };
    bsp_status_t st;
    if (BSP_spiIsBusy()) {
        return false;
    }
    (void)memset(l_scl.rx, 0, sizeof(l_scl.rx));
    st = BSP_spiTransferStart(BSP_SPI_SENSOR, zeros, l_scl.rx, (uint16_t)SCL_FRAME_LEN);
    if (st != BSP_OK) {
        return false;
    }
    l_scl.point = SCL_POINT_NONE;
    l_scl.frame_pending = true;
    return true;
}

/** @brief 取走一笔已完成的事务；未完成返回 false。 */
static bool SCL_Collect(bsp_status_t *result) {
    if (!BSP_spiResult(BSP_SPI_SENSOR, result)) {
        return false;
    }
    l_scl.last_bus_ms = BSP_uptimeMs();
    l_scl.frame_pending = false;
    return true;
}

/** @brief 作业收尾：记录错误码并把状态机推到终态。 */
static void SCL_Finish(drv_status_t result) {
    l_scl.last_error = result;
    l_scl.job = (result == DRV_OK) ? SCL_JOB_DONE : SCL_JOB_FAILED;
    l_scl.frame_pending = false;
    l_scl.point = SCL_POINT_NONE;
}

/*--------------------------------------------------------------- 数据换算 */
/** @brief 倾角换算：手册 2.5 节 `Angle[°] = raw / 2^14 * 90`，输出 0.01°。
*  NOTE (无符号)：角度寄存器是 0..65536 的无符号值（手册表 18），不是补码。 */
static int32_t SCL_RawToAngleCdeg(uint16_t raw) {
    return ((int32_t)raw * APP_CFG_SCL3300_ANG_CDEG_NUM) / APP_CFG_SCL3300_ANG_DIV;
}

/** @brief 加速度换算：按当前测量模式选灵敏度，输出 mg（1 g = 1000 mg）。 */
static int32_t SCL_RawToAccMg(int16_t raw) {
    int32_t lsb_per_g;
    switch (APP_CFG_SCL3300_MODE) {
        case 1U:  lsb_per_g = 6000;  break;  /* 手册表 2：Mode 1 = 6000 LSB/g  */
        case 2U:  lsb_per_g = 3000;  break;  /* Mode 2 = 3000 LSB/g            */
        default:  lsb_per_g = 12000; break;  /* Mode 3/4 = 12000 LSB/g         */
    }
    return ((int32_t)raw * 1000) / lsb_per_g;
}

/** @brief 温度换算：手册 6.1.2 节 `T[°C] = raw / 18.9 - 273`，输出 0.01 °C。
*  NOTE (12 位补码)：温度寄存器只有 12 位，必须先按第 11 位做符号扩展，
*   否则负温度会被当成一个很大的正数。 */
static int32_t SCL_RawToTempCC(uint16_t raw) {
    int32_t v = (int32_t)(raw & 0x0FFFU);
    if ((v & 0x0800) != 0) {
        v -= 0x1000;
    }
    return ((v * APP_CFG_SCL3300_TEMP_CC_NUM) / APP_CFG_SCL3300_TEMP_DIV)
           - APP_CFG_SCL3300_TEMP_CC_OFF;
}

/** @brief 采样点索引 → 命令。 */
static SclCmd const *SCL_PointCmd(uint8_t index) {
    switch (index) {
        case SCL_IDX_ANG_X: return &kReadAngX;
        case SCL_IDX_ANG_Y: return &kReadAngY;
        case SCL_IDX_ANG_Z: return &kReadAngZ;
        case SCL_IDX_ACC_X: return &kReadAccX;
        case SCL_IDX_ACC_Y: return &kReadAccY;
        case SCL_IDX_ACC_Z: return &kReadAccZ;
        default:            return &kReadTemp;
    }
}

/** @brief 把一次读到的原始值换算并存入结果快照。 */
static void SCL_StorePoint(uint8_t index, uint16_t raw) {
    switch (index) {
        case SCL_IDX_ANG_X: l_scl.sample.angle_cdeg[DRV_SENSOR_AXIS_X] = SCL_RawToAngleCdeg(raw); break;
        case SCL_IDX_ANG_Y: l_scl.sample.angle_cdeg[DRV_SENSOR_AXIS_Y] = SCL_RawToAngleCdeg(raw); break;
        case SCL_IDX_ANG_Z: l_scl.sample.angle_cdeg[DRV_SENSOR_AXIS_Z] = SCL_RawToAngleCdeg(raw); break;
        case SCL_IDX_ACC_X: l_scl.sample.acc_mg[DRV_SENSOR_AXIS_X] = SCL_RawToAccMg((int16_t)raw); break;
        case SCL_IDX_ACC_Y: l_scl.sample.acc_mg[DRV_SENSOR_AXIS_Y] = SCL_RawToAccMg((int16_t)raw); break;
        case SCL_IDX_ACC_Z: l_scl.sample.acc_mg[DRV_SENSOR_AXIS_Z] = SCL_RawToAccMg((int16_t)raw); break;
        case SCL_IDX_TEMP:  l_scl.sample.temp_cC = SCL_RawToTempCC(raw); break;
        default: break;
    }
}

/*--------------------------------------------------------------- 初始化推进 */
/**
* @brief 推进器件上电时序（手册 4.2）。
*
* 流程：SW Reset → 收帧 → 等 3 ms → 设模式 → 收帧 → 写 ANG_CTRL → 收帧
*       → 读 WHOAMI（判 C1h）→ 丢首帧 → 读 STATUS → READY
*/
static void SCL_InitStep(void) {
    bsp_status_t st;

    /* 第一步：如果有一笔事务在飞，先把它收掉 */
    if (l_scl.frame_pending) {
        if (!SCL_Collect(&st)) {
            return;                       /* 还没完成，下一拍再来 */
        }
        if (st != BSP_OK) {
            l_scl.last_error = DRV_ERR_BUS;
            l_scl.init = SCL_INIT_RESET;  /* 从头再来 */
            return;
        }
        switch (l_scl.init) {
            case SCL_INIT_WHOAMI:
                /* 手册 6.7：WHOAMI 固定回 C1h，用来确认 SPI 通信正常 */
                if (SCL_RxData() != SCL_WHOAMI_VALUE) {
                    l_scl.last_error = DRV_ERR_NODEV;
                    l_scl.init = SCL_INIT_RESET;
                } else {
                    l_scl.init = SCL_INIT_DISCARD;
                }
                return;
            case SCL_INIT_STATUS:
                /* STATUS「读即清」：这一帧拿到的是有效状态，存下来备查 */
                if (SCL_RxCrcOk()) {
                    l_scl.status = SCL_RxData();
                    l_scl.status_valid = true;
                }
                l_scl.init = SCL_READY;
                return;
            case SCL_INIT_DISCARD:
                /* 复位后的第一个响应已按手册丢弃 */
                l_scl.init = SCL_INIT_STATUS;
                return;
            default:
                /* RESET / SET_MODE / ANG_EN：写命令，直接进入下一阶段 */
                break;
        }
    }

    if (!SCL_GapElapsed() || BSP_spiIsBusy()) {
        return;
    }

    switch (l_scl.init) {
        case SCL_INIT_RESET:
            l_scl.status = SCL_STATUS_INVALID;
            l_scl.status_valid = false;
            l_scl.last_error = DRV_OK;
            if (SCL_Submit(&kSwReset, SCL_POINT_NONE, true)) {
                l_scl.reset_ms = BSP_uptimeMs();
                l_scl.init = SCL_INIT_AFTER_RESET;
            }
            break;

        case SCL_INIT_AFTER_RESET:
            /* 手册 4.2 第 3 步：SW Reset 后等 3 ms（存储器读取 + 信号路径稳定） */
            if ((uint32_t)(BSP_uptimeMs() - l_scl.reset_ms) >=
                (uint32_t)APP_CFG_SCL3300_RESET_WAIT_MS) {
                l_scl.init = SCL_INIT_SET_MODE;
            }
            break;

        case SCL_INIT_SET_MODE: {
            SclCmd const *cmd;
            switch (APP_CFG_SCL3300_MODE) {
                case 1U:  cmd = &kMode1; break;
                case 2U:  cmd = &kMode2; break;
                case 4U:  cmd = &kMode4; break;
                default:  cmd = &kMode3; break;   /* 默认 Mode 3 */
            }
            if (SCL_Submit(cmd, SCL_POINT_NONE, true)) {
                l_scl.init = SCL_INIT_ANG_EN;
            }
            break;
        }

        case SCL_INIT_ANG_EN:
            /* 手册 6.6：不写 ANG_CTRL 就读不到角度输出 */
            if (SCL_Submit(&kAngEnable, SCL_POINT_NONE, true)) {
                l_scl.init = SCL_INIT_WHOAMI;
            }
            break;

        case SCL_INIT_WHOAMI:
            /* 这一帧的 MISO 回的是 ANG_CTRL 的响应；真正的 WHOAMI 判定在收帧时做。
             * 收帧后若值不为 C1h 会回到 RESET 重来。 */
            (void)SCL_Submit(&kReadWho, SCL_POINT_NONE, true);
            break;

        case SCL_INIT_DISCARD:
            (void)SCL_SubmitDiscard();
            break;

        case SCL_INIT_STATUS:
            (void)SCL_Submit(&kReadStatus, SCL_POINT_NONE, true);
            break;

        default:
            break;
    }
}

/*--------------------------------------------------------------- 采样推进 */
/**
* @brief 推进一次采样作业：逐点按错帧协议「提交命令 → 取数据」。
*
* 每个采样点两帧：
*   第 1 帧 point_first = true  → 提交命令，其响应属于**上一条**命令，丢弃；
*   第 2 帧 point_first = false → 该响应才是本点的数据，校验 CRC 后换算。
*/
static void SCL_JobStep(void) {
    bsp_status_t st;

    /* 先收上一笔事务 */
    if (l_scl.frame_pending) {
        if (!SCL_Collect(&st)) {
            return;
        }
        if (st != BSP_OK) {
            SCL_Finish(DRV_ERR_BUS);
            return;
        }
        if (!l_scl.point_first) {
            /* 这是「取数据」帧：先判 RS，再校 CRC，最后换算 */
            if (SCL_RxRs() != SCL_RS_OK) {
                /* 手册 4.1：RS 指示错误时才需要去读 STATUS 定位原因。
                 * 这里不额外读 STATUS（会让本帧再多两帧事务），只记错误码，
                 * 由上层决定是否重新初始化。 */
                SCL_Finish(DRV_ERR_BUS);
                return;
            }
            if (!SCL_RxCrcOk()) {
                SCL_Finish(DRV_ERR_BUS);
                return;
            }
            SCL_StorePoint(l_scl.point, SCL_RxData());
            ++l_scl.point;
            /* NOTE (必须复位 point_first)：下一个点又是「先提交命令」，
             * 忘记复位会去取上一帧的响应，CRC 校验必然失败。 */
            l_scl.point_first = true;
            if (l_scl.point >= SCL_POINT_COUNT) {
                l_scl.sample.status = l_scl.status;
                l_scl.sample.status_valid = l_scl.status_valid;
                SCL_Finish(DRV_OK);
            }
        }
        return;
    }

    /* 再发起下一帧 */
    if (!SCL_GapElapsed() || BSP_spiIsBusy()) {
        return;
    }
    (void)SCL_Submit(SCL_PointCmd(l_scl.point), l_scl.point, l_scl.point_first);
    /* 第 1 帧提交成功后翻成第 2 帧，下一拍就会去取该点的数据。 */
    if (l_scl.point_first) {
        l_scl.point_first = false;
    }
}

/*--------------------------------------------------------------- 对外接口 */
drv_status_t DRV_Sensor_Init(void) {
    (void)memset(&l_scl, 0, sizeof(l_scl));
    l_scl.init = SCL_INIT_RESET;
    l_scl.job = SCL_JOB_NONE;
    l_scl.last_error = DRV_OK;
    l_scl.frame_pending = false;
    l_scl.point = SCL_POINT_NONE;
    l_scl.point_first = true;
    l_scl.status = SCL_STATUS_INVALID;
    l_scl.status_valid = false;
    return DRV_OK;
}

bool DRV_Sensor_IsReady(void) {
    return l_scl.init == SCL_READY;
}

void DRV_Sensor_Process(void) {
    /* 作业整体期限：跨过两个采样周期还没完就判超时，避免永远不出结果 */
    if ((l_scl.job == SCL_JOB_RUNNING) &&
        ((uint32_t)(BSP_uptimeMs() - l_scl.job_ms) >=
         (uint32_t)APP_CFG_SCL3300_JOB_TIMEOUT_MS)) {
        SCL_Finish(DRV_ERR_TIMEOUT);
        return;
    }

    if (l_scl.init != SCL_READY) {
        SCL_InitStep();
        return;
    }
    if (l_scl.job == SCL_JOB_RUNNING) {
        SCL_JobStep();
    }
}

drv_status_t DRV_Sensor_StartSample(void) {
    if (l_scl.init != SCL_READY) {
        return DRV_ERR_NODEV;
    }
    if (l_scl.job == SCL_JOB_RUNNING) {
        return DRV_ERR_BUSY;
    }
    l_scl.job = SCL_JOB_RUNNING;
    l_scl.job_ms = BSP_uptimeMs();
    l_scl.last_error = DRV_OK;
    l_scl.point = 0U;
    l_scl.point_first = true;
    l_scl.sample.status_valid = false;
    return DRV_OK;
}

bool DRV_Sensor_IsDone(void) {
    return (l_scl.job == SCL_JOB_DONE) || (l_scl.job == SCL_JOB_FAILED);
}

drv_status_t DRV_Sensor_GetSample(DrvSensorSample *out) {
    if (out == (DrvSensorSample *)0) {
        return DRV_ERR_PARAM;
    }
    if (l_scl.job != SCL_JOB_DONE) {
        return DRV_ERR_BUSY;
    }
    *out = l_scl.sample;
    l_scl.job = SCL_JOB_NONE;
    return DRV_OK;
}

drv_status_t DRV_Sensor_LastError(void) {
    return l_scl.last_error;
}
