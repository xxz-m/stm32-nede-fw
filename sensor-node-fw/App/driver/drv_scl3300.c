/*============================================================================
* drv_scl3300.c —— 器件驱动层：SCL3300-D01 三轴倾角仪（SPI，非阻塞）
*
* 层次定位：App → Service → **Driver(器件驱动层)** → BSP → HAL/CMSIS
* 目标器件：Murata SCL3300-D01；手册 Doc.No. 4921 Rev. 4。
* 片选：PA4（经 BSP_SPI_SENSOR 槽登记）；总线：共享 SPI1，Mode 0，APB2/32。
* 所属内核：QP/C 6.9.1 QV（协作式单栈）——本文件**任何函数都不得阻塞**。
*
* 手册要点（本实现全部依据这几条，改动前请回查手册原文）：
*   5.1.2 错帧协议：本次帧的 MISO 回的是**上一条命令**的结果。本实现为简化对应关系，
*         对每个采样寄存器重复发送两帧：第 1 帧提交命令（响应丢弃），第 2 帧取数据。
*         NOTE (协议与实现)：协议允许连续提交不同命令形成流水线；两帧一组是本驱动
*         的实现选择，不是所有寄存器读取都必须重复发送同一条命令。
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
*
* 阅读顺序：公开接口（文件末尾）→ SCL_InitStep / SCL_JobStep / SCL_RecoverStep
*   → SCL_Submit / SCL_Collect → 组帧、响应解析、工程量换算。
* 调用链：AO_Sensor 的 SENSOR_PROCESS_SIG → BSP_spiProcess() → DRV_Sensor_Process()。
*   BSP 只负责 DMA、片选、模式与分频；本驱动负责命令含义、响应对应和器件状态。
* NOTE (两层完成)：DMA 完成不等于总线已释放，必须由 SCL_Collect() 取走结果；
*   单帧完成也不等于整次采样完成，7 个量全部成功后才允许 GetSample() 输出快照。
*===========================================================================*/
#include "drv_scl3300.h"
#include "app_config.h"
#include "bsp.h"

#if (APP_CFG_SCL3300_TRACE != 0)
/** @brief 逐字节打印一帧，数值使用十进制；仅用于人工定位，不是采样数据接口。 */
static void SCL_TraceFrame(char const *tag, uint8_t const *frame) {
    BSP_print("scl> "); BSP_print(tag); BSP_print(" ");
    BSP_printU32(frame[0]); BSP_print(" "); BSP_printU32(frame[1]);
    BSP_print(" "); BSP_printU32(frame[2]); BSP_print(" ");
    BSP_printU32(frame[3]); BSP_printLine("");
}
#define SCL_TRACE_STATE(s) do { BSP_print("scl> state="); BSP_printLine(s); } while (0)
#else
#define SCL_TRACE_STATE(s) ((void)0)
#endif

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

/*! 手册表 15 固定写命令的低 16 位表示：高字节为动作数据低字节，低字节为示例 CRC。
 * NOTE (表示方式)：例如 Mode 3 的完整帧是 B4 00 02 25，此处保存 0x0225；
 * BuildFrame() 保留 0x0200、重新计算 CRC。这里不是可直接赋值的完整 D[23:8]。 */
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

/*! STATUS（状态摘要）位定义，手册 6.3 表 27 逐位照抄；不是自研约定。
*  NOTE (为什么必须逐位分清，2026-10-14 现场实测)：手册 5.1.5 明确「状态摘要寄存器里
*   有**任何**标志活动时，RS 位显示错误（'11'）」。也就是说哪怕只锁存了复位标志，
*   之后每一条应答都带 RS=11。把「良性位」当成器件故障去整片重初始化，就会陷入
*   「复位 → 改模式 → 置位 → 采样被判错 → 又复位」的死循环（用户实测正是如此）。 */
#define SCL_STATUS_PIN_CONTINUITY (1U << 0)  /* 器件内部连接错误，可能器件故障 */
#define SCL_STATUS_MODE_CHANGE    (1U << 1)  /* 模式被改过：我们自己写模式时必然置位 */
#define SCL_STATUS_PD             (1U << 2)  /* 未请求的掉电 */
#define SCL_STATUS_MEM            (1U << 3)  /* 非易失存储器错误 */
#define SCL_STATUS_PWR            (1U << 4)  /* 复位后置位属正常（手册注明无需处理） */
#define SCL_STATUS_TEMP_SAT       (1U << 5)  /* 温度信号路径饱和 */
#define SCL_STATUS_SAT            (1U << 6)  /* 信号路径饱和：本帧数据不可用 */
#define SCL_STATUS_CLK            (1U << 7)  /* 时钟错误 */
#define SCL_STATUS_DIGI2          (1U << 8)  /* 数字块错误 2 */
#define SCL_STATUS_DIGI1          (1U << 9)  /* 数字块错误 1 */

/*! 手册表 27 里明写「需要软件或硬件复位」的位：只有这些才值得整片重初始化。
*  NOTE (SAT/TEMP_SAT 不入列)：它们是**量程/温度条件**位，不是器件故障；出现时本帧
*   数据无效（由 RS=11 拦下），但不能靠复位器件解决。 */
#define SCL_STATUS_FAULT_MASK \
    (SCL_STATUS_DIGI1 | SCL_STATUS_DIGI2 | SCL_STATUS_CLK | SCL_STATUS_MEM | \
     SCL_STATUS_PD | SCL_STATUS_PIN_CONTINUITY)

/*! 状态摘要读清的上限帧数。手册 6.3.1 的例子连读三帧才返回 0x0000，这里留足余量。 */
#define SCL_STATUS_READ_MAX  (8U)

/*! 单个采样点因 RS≠01 允许的重做次数（先读清摘要再重做本点）。 */
#define SCL_POINT_RETRY_MAX  (2U)

/*! 还没读到过 STATUS。 */
#define SCL_STATUS_INVALID  (0xFFFFU)

/*! 帧长度：4 字节（32 位）。 */
#define SCL_FRAME_LEN       (4U)

/*! 采样点数：X/Y/Z 角度 + X/Y/Z 加速度 + 温度。 */
#define SCL_POINT_COUNT     (7U)

/*! `point` 的哨兵值：表示「当前没有在等某个采样点的响应」。 */
#define SCL_POINT_NONE      (0xFFU)

/*--------------------------------------------------------------- 命令表 */
/*! 以「地址 + 固定命令尾部 + 读/写」描述命令；帧的 CRC 在运行期重新计算。 */
typedef struct {
    uint8_t  addr;
    uint16_t data;            /* 固定命令低 16 位表示，布局见上方 SCL_DATA_* 说明 */
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
/* init 与 job 是两套独立状态：init 管器件能否采样，job 管一份结果是否待取。
 * 异常时可以同时处于「器件正在恢复」与「旧作业失败待取」，取走失败结果不会结束恢复。 */
typedef enum {
    SCL_INIT_RESET = 0,   /* 发 SW Reset                     */
    SCL_INIT_AFTER_RESET, /* 等手册 4.2 的 3 ms              */
    SCL_INIT_SET_MODE,    /* 写测量模式                      */
    SCL_INIT_ANG_EN,      /* 写 ANG_CTRL 使能角度            */
    SCL_INIT_ANG_EN_RESP, /* 丢弃 ANG_CTRL 写命令的响应      */
    SCL_INIT_WHOAMI,      /* 发起 WHOAMI 读取命令            */
    SCL_INIT_WHOAMI_RESP, /* 取 WHOAMI 实际响应              */
    SCL_INIT_DISCARD,     /* 历史名称：当前仅等待模式稳定，不发帧 */
    SCL_INIT_STATUS,      /* 提交一次 STATUS 读（把状态摘要读清） */
    SCL_INIT_STATUS2,     /* 收取并判读上一条命令的应答，决定继续读还是进 WHOAMI */
    SCL_RECOVER_STATUS,   /* 运行期异常：连续四帧读取并清除 STATUS */
    SCL_RECOVER_WAIT,     /* 清除失败：延迟后重新初始化传感器 */
    SCL_READY
} SclInitState;

typedef enum {
    SCL_JOB_NONE = 0,     /* 无待取作业                     */
    SCL_JOB_RUNNING,      /* 逐帧读取中，sample 尚不完整     */
    SCL_JOB_DONE,         /* 完整快照待 GetSample 取走       */
    SCL_JOB_FAILED        /* 错误码待 GetSample 取走，不输出半份快照 */
} SclJobState;

typedef struct {
    SclInitState init;
    SclJobState  job;
    drv_status_t last_error;

    bool     frame_pending;   /* true = 已发起一笔事务，等 BSP 完成 */
    uint8_t  point;           /* 当前采样点索引；SCL_POINT_NONE = 无 */
    bool     point_first;     /* true = 该点的第 1 帧（提交命令）    */
    uint8_t  retry_stage;     /* 本点重做：1=发 STATUS 读清、2=本点命令帧、3=本点数据帧、0=正常 */
    uint8_t  point_retry;     /* 本点已重做次数（成功存点后归零）    */
    uint8_t  recovery_frame;  /* 本轮已收取的 STATUS 帧数 */
    uint8_t  status_reads;    /* 初始化阶段读清 STATUS 已提交的读次数（有界） */
    bool     recovery_reset;  /* 出现真故障位，读清不足以恢复 */

    uint32_t last_bus_ms;     /* 上一笔事务结束的时刻（帧间隔基准）  */
    uint32_t reset_ms;        /* SW Reset 发出的时刻（等 3 ms）      */
    uint32_t job_ms;          /* 本次采样作业的起始时刻              */

    /* DMA 异步访问，缓冲必须跨 Process 调用保持有效，不能换成 Submit 的局部数组。 */
    uint8_t  tx[SCL_FRAME_LEN];
    uint8_t  rx[SCL_FRAME_LEN];

    /* 最近一次初始化/恢复读到的状态，不是每次采样都重新读取的实时 STATUS。 */
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
    /* 当前固定命令的 D 高字节均为 0；data 已包含动作字节的位置，不能再左移 8 位。
     * 例如读 ANG_X：地址 0x09 << 2 = 首字节 0x24，组帧后为 24 00 00 C7。 */
    uint32_t val = ((op << 26) | ((uint32_t)cmd->data & 0xFFFFU)) & 0xFFFFFF00UL;
    uint32_t frame = val | (uint32_t)SCL_CalcCrc(val);
    out[0] = (uint8_t)(frame >> 24);
    out[1] = (uint8_t)(frame >> 16);
    out[2] = (uint8_t)(frame >> 8);
    out[3] = (uint8_t)(frame);
}

/*--------------------------------------------------------------- 响应解析 */
/** @brief 取数据字段 D[23:8]：rx[1] 为高字节，rx[2] 为低字节，与 MCU 内存端序无关。 */
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
    /* 使用无符号差值，可跨毫秒计数器回绕；实际发帧还受 AO 推进周期和 Flash 占用影响。 */
    return (uint32_t)(BSP_uptimeMs() - l_scl.last_bus_ms) >=
           (uint32_t)APP_CFG_SCL3300_FRAME_GAP_MS;
}

/**
* @brief 发起一帧（一次 4 字节 SPI 事务），随后立刻返回。
* @param cmd 要发送的命令
* @param point 该响应归属的采样点；SCL_POINT_NONE = 与采样无关（初始化用）
* @param first true = 该点的第 1 帧（提交命令）；false = 第 2 帧（取数据）
* @return true = BSP 已受理，不表示数据已读回；false = 本拍未发出，后续 Process 再试
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
        SCL_TRACE_STATE("spi-start-fail");
        return false;
    }
#if (APP_CFG_SCL3300_TRACE != 0)
    SCL_TraceFrame("tx", l_scl.tx);
#endif
    l_scl.point = point;
    l_scl.point_first = first;
    l_scl.frame_pending = true;
    return true;
}

/** @brief 取走已完成事务并释放 SPI 所有权；未完成返回 false，result 接收总线结果。 */
static bool SCL_Collect(bsp_status_t *result) {
    /* BSP_spiResult 只允许 SENSOR 取走自己的结果，不能替 Flash 消费完成状态。 */
    if (!BSP_spiResult(BSP_SPI_SENSOR, result)) {
        return false;
    }
#if (APP_CFG_SCL3300_TRACE != 0)
    SCL_TraceFrame("rx", l_scl.rx);
#endif
    l_scl.last_bus_ms = BSP_uptimeMs();
    l_scl.frame_pending = false;
    return true;
}

/** @brief 作业收尾：记录错误码并进入终态；调用前必须已收回事务，本函数不释放 BSP 所有权。 */
static void SCL_Finish(drv_status_t result) {
    l_scl.last_error = result;
    l_scl.job = (result == DRV_OK) ? SCL_JOB_DONE : SCL_JOB_FAILED;
    l_scl.frame_pending = false;
    l_scl.point = SCL_POINT_NONE;
}

/** @brief 已收回事务后放弃本次样本，切换到 STATUS 清除流程。 */
static void SCL_StartRecovery(void) {
    SCL_Finish(DRV_ERR_BUS);
    l_scl.recovery_frame = 0U;
    l_scl.recovery_reset = false;
    l_scl.status_valid = false;
    /* 重做本点的中间状态必须一起清掉，否则恢复结束后会带着半截重做序列继续采样。 */
    l_scl.retry_stage = 0U;
    l_scl.point_retry = 0U;
    l_scl.init = SCL_RECOVER_STATUS;
    SCL_TRACE_STATE("reading-status");
}

/** @brief 延迟重启传感器；此时没有未收取的 SPI 事务，不影响 Flash 所有权。 */
static void SCL_RetryLater(void) {
    l_scl.reset_ms = BSP_uptimeMs();
    l_scl.init = SCL_RECOVER_WAIT;
    SCL_TRACE_STATE("sensor-reinit-pending");
}

/** @brief 按手册 6.3.1 的四帧示例清除锁存错误；异常数据始终不向上提交。 */
static void SCL_RecoverStep(void) {
    bsp_status_t st;
    if (l_scl.frame_pending) {
        if (!SCL_Collect(&st)) {
            return;
        }
        if (st != BSP_OK) {
            SCL_RetryLater();
            return;
        }
        ++l_scl.recovery_frame;
        /* 第一帧仍属于之前的采样命令；后续帧才能解释为 STATUS。
         * 连续发送四帧：提交 STATUS → 收锁存值 → 收清除过渡值 → 检查最终状态。
         * 读清过程中旧错误仍可能出现在响应中，因此不要求中间帧的 RS 立即正常。 */
        if (l_scl.recovery_frame > 1U) {
            if (!SCL_RxCrcOk() ||
                ((l_scl.rx[0] & 0xFCU) != (SCL_ADDR_STATUS << 2))) {
                SCL_RetryLater();
                return;
            }
            l_scl.status = SCL_RxData();
            l_scl.status_valid = true;
            /* NOTE (判据改成故障位掩码，2026-10-14)：原先是「除 SAT 外任何位都判故障」，
             *   而实测锁存的正是 0x12 = PWR(bit4) + MODE_CHANGE(bit1)：
             *     - PWR：手册表 27 写明「复位后此标志置高，**无需操作**」；
             *     - MODE_CHANGE：手册表 27 定义为「模式已被改变」，而模式正是**我们自己**
             *       在初始化/重初始化里写的。
             *   两者都已经被读清（实测第三帧读到 0x0000），却因为这一条被当成器件故障，
             *   于是每一轮采样失败都要再整片重初始化一次——重初始化又会重新置位这两个位，
             *   形成永不收敛的循环。改成只看手册里「需要软件或硬件复位」的真故障位。 */
            if ((l_scl.status & SCL_STATUS_FAULT_MASK) != 0U) {
                l_scl.recovery_reset = true;
            }
#if (APP_CFG_SCL3300_TRACE != 0)
            BSP_print("scl> status_dec="); BSP_printU32(l_scl.status);
            BSP_print(" rs="); BSP_printU32(SCL_RxRs()); BSP_printLine("");
#endif
        }
        if (l_scl.recovery_frame == 4U) {
            if (!l_scl.recovery_reset && SCL_RxRs() == SCL_RS_OK && l_scl.status == 0U) {
                l_scl.init = SCL_READY;
                SCL_TRACE_STATE("status-cleared");
            } else {
                SCL_RetryLater();
            }
        }
        return;
    }
    if (SCL_GapElapsed() && !BSP_spiIsBusy()) {
        (void)SCL_Submit(&kReadStatus, SCL_POINT_NONE, true);
    }
}

/*--------------------------------------------------------------- 数据换算 */
/** @brief 倾角换算：16位补码先符号扩展，再按 raw * 9000 / 16384 输出0.01°。
*  NOTE (过零)：必须在批内平均前保留负号；否则负小角度变成约360°，四点平均会产生90°假跳变。
*  显式减65536避免无符号转窄有符号的实现相关行为，不做零点扣除或范围裁剪。 */
static int32_t SCL_RawToAngleCdeg(uint16_t raw) {
    int32_t signed_raw = raw < 0x8000U ? (int32_t)raw : (int32_t)raw - 65536;
    return (signed_raw * APP_CFG_SCL3300_ANG_CDEG_NUM) / APP_CFG_SCL3300_ANG_DIV;
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

/** @brief 温度换算：`T[°C] = TEMP / 18.9 - 273`，输出 0.01 °C。
*  NOTE (16 位补码)：当前按完整 int16_t 解释 TEMP，不能截成 12 位。
*  整数公式为 raw * 10000 / 1890 - 27300；分母少一个 0 会出现数千度假读数。
*  例如 raw=5634 → 2509，即 25.09°C；先乘后除保留小数精度，最终整数除法截断。 */
static int32_t SCL_RawToTempCC(uint16_t raw) {
    int32_t v = (int32_t)(int16_t)raw;
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
        case SCL_IDX_TEMP:
            l_scl.sample.temp_cC = SCL_RawToTempCC(raw);
#if (APP_CFG_SCL3300_TRACE != 0)
            /* NOTE (追踪显示)：下方打印函数输出十进制，历史标签中的 0x 不代表十六进制。 */
            BSP_print("scl> temp raw=0x");
            BSP_printU32((uint32_t)raw);
            BSP_print(" raw16=");
            BSP_printU32((uint32_t)(uint16_t)raw);
            BSP_print(" cc=");
            BSP_printU32((uint32_t)l_scl.sample.temp_cC);
            BSP_printLine("");
#endif
            break;
        default: break;
    }
}

/*--------------------------------------------------------------- 初始化推进 */
/**
* @brief 推进器件上电时序（手册 4.2）。
*
* 流程：SW Reset → 收帧 → 等 3 ms → 设模式 → 收帧 → 写 ANG_CTRL → 收帧
*       → 等模式稳定 → 连续读 STATUS → 读 WHOAMI（判 C1h）→ READY
* NOTE (状态含义)：发帧后有些状态已提前切到下一阶段；进入函数先看 frame_pending，
*   收掉上一笔事务再决定是否推进。不能仅凭枚举名称认定 rx 对应哪条命令。
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
            SCL_RetryLater();
            return;
        }
        switch (l_scl.init) {
            case SCL_INIT_WHOAMI:
                /* WHOAMI 第一帧只提交读取命令，响应仍属于上一条命令。 */
                l_scl.init = SCL_INIT_WHOAMI_RESP;
                return;
            case SCL_INIT_WHOAMI_RESP:
                /* 手册 6.7：第二帧返回 WHOAMI，固定应为 C1h。 */
#if (APP_CFG_SCL3300_TRACE != 0)
                BSP_print("scl> whoami="); BSP_printU32(SCL_RxData()); BSP_printLine("");
#endif
                if (!SCL_RxCrcOk() || SCL_RxRs() != SCL_RS_OK ||
                    SCL_RxData() != SCL_WHOAMI_VALUE) {
                    l_scl.last_error = DRV_ERR_NODEV;
                    SCL_RetryLater();
                } else {
                    l_scl.init = SCL_READY;
                    SCL_TRACE_STATE("ready-whoami-193");
                }
                return;
            case SCL_INIT_STATUS2: {
                /* 错帧协议：这里收到的是**上一条命令**的应答。第一次进入时上一条是 ANG_CTRL
                 * 写命令（回显地址是 0x0C），从第二次起才是 STATUS 摘要本身。
                 *
                 * NOTE (为什么读到 0 才罢休，2026-10-14 现场实测 + 手册 6.3.1)：只要状态摘要
                 *   里还有**任何**一位，器件就会在**每一条**应答里返回 RS=11（手册 5.1.5），
                 *   于是采样的第一个数据帧必然被判成错误。实测 0x12 = PWR(bit4)+MODE_CHANGE
                 *   (bit1)，两帧回显旧值、第三帧才回到 0x0000。旧实现只发三次读，并且把最后
                 *   那个「已经清零」的应答丢在 WHOAMI 那一拍（错帧协议下它晚到一步），
                 *   因此 READY 时摘要仍可能是脏的，而 status 记的是清除前的旧值。 */
                bool const isStatus = SCL_RxCrcOk() &&
                    ((l_scl.rx[0] & 0xFCU) == (uint8_t)(SCL_ADDR_STATUS << 2));
                if (!isStatus) {
                    /* 不是 STATUS 的应答（ANG_CTRL 回显、或本帧 CRC 坏）：不算有效读，继续读。 */
                    if (l_scl.status_reads >= SCL_STATUS_READ_MAX) {
                        l_scl.last_error = DRV_ERR_BUS;
                        SCL_RetryLater();
                        return;
                    }
                    l_scl.init = SCL_INIT_STATUS;
                    return;
                }
                l_scl.status = SCL_RxData();
                l_scl.status_valid = true;
                if ((l_scl.status == 0U) && (SCL_RxRs() == SCL_RS_OK)) {
                    l_scl.init = SCL_INIT_WHOAMI;      /* 摘要干净，去核对器件身份 */
                    return;
                }
                if (((l_scl.status & SCL_STATUS_FAULT_MASK) != 0U) ||
                    (l_scl.status_reads >= SCL_STATUS_READ_MAX)) {
                    /* 真故障位，或读满上限仍未清零：交给恢复流程重新初始化器件。 */
                    l_scl.last_error = DRV_ERR_BUS;
                    SCL_RetryLater();
                    SCL_TRACE_STATE("status-not-clear");
                    return;
                }
                l_scl.init = SCL_INIT_STATUS;          /* 还有残留标志（例如复位后的 PWR）：再读一次 */
                return;
            }
            case SCL_INIT_ANG_EN_RESP:
                /* ANG_CTRL 写命令的响应属于上一帧，不能当作 WHOAMI。 */
                l_scl.reset_ms = BSP_uptimeMs();
                l_scl.init = SCL_INIT_DISCARD;
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
                l_scl.init = SCL_INIT_ANG_EN_RESP;
            }
            break;

        case SCL_INIT_WHOAMI:
            if (SCL_Submit(&kReadWho, SCL_POINT_NONE, true)) {
                /* 保持 WHOAMI 状态；本次收帧是提交帧的响应，下一拍再发第二帧。 */
            }
            break;

        case SCL_INIT_WHOAMI_RESP:
            /* 第二帧取回真正的 WHOAMI 数据。 */
            (void)SCL_Submit(&kReadWho, SCL_POINT_NONE, false);
            break;

        case SCL_INIT_DISCARD: {
            /* 此状态只检查模式稳定期限；reset_ms 在 ANG_CTRL 事务收回后更新，
             * 因此此处计时起点与软件复位后的 3 ms 等待不同。 */
            uint32_t wait = (APP_CFG_SCL3300_MODE == 1U) ? 25U :
                            (APP_CFG_SCL3300_MODE == 2U) ? 15U : 100U;
            if ((uint32_t)(BSP_uptimeMs() - l_scl.reset_ms) >= wait) {
                l_scl.status_reads = 0U;   /* 读清计数从进入 STATUS 阶段起算 */
                l_scl.init = SCL_INIT_STATUS;
            }
            break;
        }
        case SCL_INIT_STATUS:
            /* 每次只提交一条 STATUS 读；清零判定放在下一拍收到的应答上，读到 0 为止。 */
            if (SCL_Submit(&kReadStatus, SCL_POINT_NONE, true)) {
                if (l_scl.status_reads < 0xFFU) { ++l_scl.status_reads; }
                l_scl.init = SCL_INIT_STATUS2;
            }
            break;
        case SCL_INIT_STATUS2:
            /* 提交分支不会停在判读态（判读态恒有一笔在飞）；留空以防意外落到这里。 */
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
            SCL_StartRecovery();
            return;
        }
        if (!l_scl.point_first) {
            /* 先验证 CRC：帧内不一致属于总线/信号完整性问题，重做本点没有意义，直接走恢复。 */
            if (!SCL_RxCrcOk()) {
                SCL_StartRecovery();
                return;
            }
            if (SCL_RxRs() != SCL_RS_OK) {
                /* NOTE (RS≠01 不再直接作废整批，2026-10-14 现场实测)：手册 5.1.5 规定
                 *   「状态摘要寄存器中有**任何**标志活动时，RS 位显示错误（'11'）」，
                 *   而摘要里可能只锁存了我们自己造成的位——复位后的 PWR（表 27：置位属
                 *   正常、无需处理）与改模式后的 MODE_CHANGE。手册 6.3 又说明摘要读一次
                 *   即复位。旧实现一遇到 RS≠01 就判整批失败，于是每个上报周期都白丢一次
                 *   采样（用户实测就是这个现象）。这里改成：先插一条 STATUS 读把摘要读清，
                 *   再重做本点，最多 SCL_POINT_RETRY_MAX 次；仍失败才进恢复流程。 */
                if (l_scl.point_retry < SCL_POINT_RETRY_MAX) {
                    ++l_scl.point_retry;
                    l_scl.retry_stage = 1U;
                    return;
                }
                SCL_StartRecovery();
                return;
            }
            l_scl.point_retry = 0U;    /* 本点成功，重做预算清零 */
            SCL_StorePoint(l_scl.point, SCL_RxData());
            ++l_scl.point;
            /* NOTE (必须复位 point_first)：下一个点又是「先提交命令」。
             * 忘记复位会把上一个寄存器的响应存入下一个量；旧响应的 CRC 仍可能正确，
             * 因此 CRC 通过只能证明帧内一致，不能证明命令与数据对应正确。 */
            l_scl.point_first = true;
            if (l_scl.point >= SCL_POINT_COUNT) {
                l_scl.sample.status = l_scl.status;
                l_scl.sample.status_valid = l_scl.status_valid;
                SCL_Finish(DRV_OK);
            }
        } else {
            /* 第一帧收完才能切换为取数据帧。 */
            l_scl.point_first = false;
        }
        return;
    }

    /* 再发起下一帧 */
    if (!SCL_GapElapsed() || BSP_spiIsBusy()) {
        return;
    }
    /* 重做本点的三帧序列：① STATUS 读（把摘要读清，其应答在下一帧被丢弃）
     * ② 本点命令帧（回显丢弃）③ 本点数据帧（真正判读）。错帧协议下缺一不可。 */
    switch (l_scl.retry_stage) {
        case 1U:
            l_scl.retry_stage = 2U;
            (void)SCL_Submit(&kReadStatus, l_scl.point, true);
            return;
        case 2U:
            l_scl.retry_stage = 3U;
            (void)SCL_Submit(SCL_PointCmd(l_scl.point), l_scl.point, true);
            return;
        case 3U:
            l_scl.retry_stage = 0U;
            (void)SCL_Submit(SCL_PointCmd(l_scl.point), l_scl.point, false);
            return;
        default:
            break;
    }
    (void)SCL_Submit(SCL_PointCmd(l_scl.point), l_scl.point, l_scl.point_first);

}

/*--------------------------------------------------------------- 对外接口 */
/** @brief 重置软件上下文；仅在启动且无本驱动未收取 SPI 事务时调用，不同步复位硬件。 */
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

/** @brief 推进恢复、初始化或采样其中一小步；调用方须先推进 BSP_spiProcess。 */
void DRV_Sensor_Process(void) {
    bsp_status_t st;
    if (l_scl.init == SCL_RECOVER_STATUS) {
        SCL_RecoverStep();
        return;
    }
    if (l_scl.init == SCL_RECOVER_WAIT) {
        if ((uint32_t)(BSP_uptimeMs() - l_scl.reset_ms) >=
            APP_CFG_SCL3300_RECOVERY_DELAY_MS) {
            l_scl.init = SCL_INIT_RESET;
        }
        return;
    }
    /* 作业整体期限由 JOB_TIMEOUT_MS 决定（当前 1500 ms），不等同于单帧 DMA 超时。 */
    if ((l_scl.job == SCL_JOB_RUNNING) &&
        ((uint32_t)(BSP_uptimeMs() - l_scl.job_ms) >=
         (uint32_t)APP_CFG_SCL3300_JOB_TIMEOUT_MS)) {
        /* 不能丢弃未收取的 BSP 所有权，否则 Flash 和传感器都会一直 BUSY。 */
        if (l_scl.frame_pending && !SCL_Collect(&st)) {
            return;
        }
        SCL_Finish(DRV_ERR_TIMEOUT);
        SCL_RetryLater();
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

/** @brief 建立一份新采样作业，首帧由后续 Process 发起；调用方应先取走上次终态结果。 */
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
    l_scl.retry_stage = 0U;    /* 新作业从第一个点正常开始，不继承上一次的重做序列 */
    l_scl.point_retry = 0U;
    l_scl.sample.status_valid = false;
    return DRV_OK;
}

bool DRV_Sensor_IsDone(void) {
    return (l_scl.job == SCL_JOB_DONE) || (l_scl.job == SCL_JOB_FAILED);
}

/** @brief 消费成功快照或失败错误码；只有 DRV_OK 才写 out，消费后清除作业终态。 */
drv_status_t DRV_Sensor_GetSample(DrvSensorSample *out) {
    drv_status_t result;
    if (out == (DrvSensorSample *)0) {
        return DRV_ERR_PARAM;
    }
    if (l_scl.job == SCL_JOB_FAILED) {
        /* 失败也必须消费；否则上层会反复看到同一终态，不能把失败误报为仍在 BUSY。 */
        result = l_scl.last_error;
        l_scl.job = SCL_JOB_NONE;
        return result;
    }
    if (l_scl.job != SCL_JOB_DONE) {
        return DRV_ERR_BUSY;
    }
    *out = l_scl.sample;
    l_scl.job = SCL_JOB_NONE;
    return DRV_OK;
}

/** @brief 查询最近保存的错误，不消费作业；新采样或重新初始化时会重置，非永久故障记录。 */
drv_status_t DRV_Sensor_LastError(void) {
    return l_scl.last_error;
}
