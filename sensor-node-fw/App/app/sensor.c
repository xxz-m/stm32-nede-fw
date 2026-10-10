/*============================================================================
* sensor.c —— 应用层：AO_Sensor 实现（SCL3300-D01 三轴倾角）
*
* 层次定位：App 层。链路演示：App → Service（滤波 + 组帧）→ Driver（读器件）
*           → BSP（共享 SPI1 与时间基准）。
*
* 数据来源：SCL3300-D01，Mode 3（±10°、10 Hz LPF）。器件驱动是**非阻塞**的：
*   AO 每 20 ms 调一次 DRV_Sensor_Process() 推进它的状态机，
*   每 1 s 发起一次 DRV_Sensor_StartSample()，完成后取结果。
*
* 状态机：
*
*   Idle ---- SYS_SENSOR_START_SIG ----> Sampling
*   Idle <--- SYS_SENSOR_STOP_SIG ------ Sampling
*   两态都处理 SYS_SENSOR_REPORT_SIG（立即上报一次，不改变状态）
*
* NOTE (2026-10-10 换掉仿真源)：原先的采样实现读的是仿真温湿度（drv_sensor.c）。
*   接真实 SCL3300 后：仿真实现已删除、湿度通道移除，改为三轴倾角 + 加速度 +
*   片内温度；采样由「一次调用直接出结果」改为「发起 → 多拍推进 → 取结果」。
*===========================================================================*/
#include "sensor.h"
#include "app_config.h"
#include "app_signals.h"
#include "app_prio.h"
#include "bsp.h"
#include "drv_scl3300.h"
#include "svc_frame.h"
#include "svc_log.h"
#include "ble.h"
#include "modem.h"

/* 本模块在 QP 断言里的名字 */
Q_DEFINE_THIS_MODULE("sensor")

/* QP/C 断言钩子（实现在 App/bsp/bsp.c） */
void Q_onAssert(char const * const module, int loc);

/*! 推进驱动的周期换算成 QP 节拍（至少 1 拍：算出 0 会变成每拍都跑）。 */
#define SENSOR_PROCESS_TICKS \
    (((APP_CFG_SCL3300_PROCESS_PERIOD_MS * BSP_TICKS_PER_SEC) / 1000U) > 0U \
         ? ((APP_CFG_SCL3300_PROCESS_PERIOD_MS * BSP_TICKS_PER_SEC) / 1000U) \
         : 1U)

/* AO 实例与事件队列（队列存储归 AO 自己所有，整改项 5） */
static Sensor l_sensor;
QActive * const AO_Sensor = &l_sensor.super;
static QEvt const *l_sensorQueueSto[8];

/*--------------------------------------------------------------------------*/
/* 本文件内部辅助函数                                                        */
/*--------------------------------------------------------------------------*/

/** @brief 打印带符号的定点数（例如 0.01 标度的 -1234 -> "-12.34"）。 */
static void printSignedFixed(int32_t value, uint32_t decimals) {
    int32_t divisor = 1;
    uint32_t i;

    for (i = 0U; i < decimals; ++i) {
        divisor *= 10;
    }
    if (value < 0) {
        BSP_print("-");
        value = -value;
    }
    BSP_printU32((uint32_t)(value / divisor));
    BSP_print(".");
    {
        int32_t frac = value % divisor;
        int32_t limit = divisor / 10;
        while ((limit > 1) && (frac < limit)) {   /* 补前导零 */
            BSP_print("0");
            limit /= 10;
        }
        BSP_printU32((uint32_t)frac);
    }
}

/** @brief 把 Driver 状态码转成可读文本（只用于打印）。 */
static char const *drvStr(drv_status_t st) {
    switch (st) {
        case DRV_OK:          return "DRV_OK";
        case DRV_ERR_PARAM:   return "DRV_ERR_PARAM";
        case DRV_ERR_BUS:     return "DRV_ERR_BUS";
        case DRV_ERR_TIMEOUT: return "DRV_ERR_TIMEOUT";
        case DRV_ERR_NODEV:   return "DRV_ERR_NODEV";
        case DRV_ERR_BUSY:    return "DRV_ERR_BUSY";
        default:              return "DRV_ERR_UNKNOWN";
    }
}

/** @brief 把 Service 状态码转成可读文本（只用于打印）。 */
static char const *svcStr(svc_status_t st) {
    switch (st) {
        case SVC_OK:         return "SVC_OK";
        case SVC_ERR_PARAM:  return "SVC_ERR_PARAM";
        case SVC_ERR_RANGE:  return "SVC_ERR_RANGE";
        case SVC_ERR_CRC:    return "SVC_ERR_CRC";
        case SVC_ERR_STATE:  return "SVC_ERR_STATE";
        case SVC_ERR_NOMEM:  return "SVC_ERR_NOMEM";
        case SVC_ERR_BUSY:   return "SVC_ERR_BUSY";
        default:             return "SVC_ERR_UNKNOWN";
    }
}

/** @brief 把 int16 按小端写进载荷。 */
static void putI16LE(uint8_t *dst, int16_t value) {
    uint16_t u = (uint16_t)value;
    dst[0] = (uint8_t)(u & 0xFFU);
    dst[1] = (uint8_t)((u >> 8) & 0xFFU);
}

/** @brief 把 uint16 按小端写进载荷。 */
static void putU16LE(uint8_t *dst, uint16_t value) {
    dst[0] = (uint8_t)(value & 0xFFU);
    dst[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/** @brief 把 32 位量夹到 int16 范围（上报载荷用 16 位节省空间）。 */
static int16_t clampI16(int32_t v) {
    if (v > 32767) { return 32767; }
    if (v < -32768) { return -32768; }
    return (int16_t)v;
}

/**
* @brief 取回一次采样结果，滤波、组帧、自检并打印。
*
* 这是本工程里「四层协作」的最小完整样例：
*   DRV_Sensor_*  （Driver：器件语义与工程换算）
*   SVC_Filter_*  （Service：无状态纯函数 + 调用方持有状态）
*   SVC_Frame_*   （Service：组帧与校验）
*   BSP_print*    （BSP：唯一的输出通道）
*/
static void consumeSample(Sensor * const me) {
    DrvSensorSample s;
    drv_status_t dst;
    svc_status_t sst;
    int32_t angFilt[DRV_SENSOR_AXIS_COUNT];
    uint8_t payload[SENSOR_REPORT_PAYLOAD_LEN];
    uint8_t frame[SVC_FRAME_MAX_SIZE];
    size_t  frameLen = 0U;
    uint8_t cmdBack = 0U;
    uint8_t payloadBack[SENSOR_REPORT_PAYLOAD_LEN];
    size_t  payloadBackLen = 0U;
    uint8_t axis;

    dst = DRV_Sensor_GetSample(&s);
    if (dst != DRV_OK) {
        ++me->errors;
        BSP_print("sensor> get sample failed: ");
        BSP_printLine(drvStr(dst));
        return;
    }

    /* 滑动平均平滑：三轴角度各自一个窗口，状态由本 AO 持有 */
    for (axis = 0U; axis < (uint8_t)DRV_SENSOR_AXIS_COUNT; ++axis) {
        angFilt[axis] = SVC_Filter_MavgPush(&me->angFilt[axis], s.angle_cdeg[axis]);
        me->angLast_cdeg[axis] = angFilt[axis];
        me->accLast_mg[axis] = s.acc_mg[axis];
    }
    me->tempLast_cC = s.temp_cC;
    me->statusLast = s.status;
    ++me->seq;

    /* STATUS 的 SAT 位（手册 6.3 表 27）：信号路径饱和时本帧数据不可用 */
    if (s.status_valid && ((s.status & 0x0040U) != 0U)) {
        BSP_print("sensor> status SAT set, data may be invalid: 0x");
        BSP_printU32((uint32_t)s.status);
        BSP_printLine("");
    }

#if (APP_CFG_BLE_ENABLE != 0)
    /* 采样值以动态事件快照交给 AO_Ble，双方不共享可变对象。 */
    {
        BleMeasurementEvt *snapshot = Q_NEW(BleMeasurementEvt, BLE_MEASUREMENT_SIG);
        snapshot->temperature_cC = s.temp_cC;
        snapshot->angle_x_cdeg = angFilt[DRV_SENSOR_AXIS_X];
        snapshot->angle_y_cdeg = angFilt[DRV_SENSOR_AXIS_Y];
        snapshot->angle_z_cdeg = angFilt[DRV_SENSOR_AXIS_Z];
        QACTIVE_POST(AO_Ble, &snapshot->super, me);
    }
#endif
#if (APP_CFG_MODEM_ENABLE != 0)
    {
        ModemMeasurementEvt *snapshot = Q_NEW(ModemMeasurementEvt, MODEM_MEASUREMENT_SIG);
        snapshot->temperature_cC = s.temp_cC;
        snapshot->angle_x_cdeg = angFilt[DRV_SENSOR_AXIS_X];
        snapshot->angle_y_cdeg = angFilt[DRV_SENSOR_AXIS_Y];
        snapshot->angle_z_cdeg = angFilt[DRV_SENSOR_AXIS_Z];
        QACTIVE_POST(AO_Modem, &snapshot->super, me);
    }
#endif

    /* 组帧（Service）：三轴倾角 int16 + 温度 int16 + 三轴加速度 int16 + STATUS u16 */
    putI16LE(&payload[0],  clampI16(angFilt[DRV_SENSOR_AXIS_X]));
    putI16LE(&payload[2],  clampI16(angFilt[DRV_SENSOR_AXIS_Y]));
    putI16LE(&payload[4],  clampI16(angFilt[DRV_SENSOR_AXIS_Z]));
    putI16LE(&payload[6],  clampI16(s.temp_cC));
    putI16LE(&payload[8],  clampI16(s.acc_mg[DRV_SENSOR_AXIS_X]));
    putI16LE(&payload[10], clampI16(s.acc_mg[DRV_SENSOR_AXIS_Y]));
    putI16LE(&payload[12], clampI16(s.acc_mg[DRV_SENSOR_AXIS_Z]));
    putU16LE(&payload[14], s.status_valid ? s.status : 0U);

    sst = SVC_Frame_Pack(SENSOR_CMD_REPORT, payload, sizeof(payload),
                         frame, sizeof(frame), &frameLen);

    /* 立即解析回来做自检（无硬件环境下唯一能验证链路正确性的手段） */
    if (sst == SVC_OK) {
        sst = SVC_Frame_Parse(frame, frameLen, &cmdBack,
                              payloadBack, sizeof(payloadBack), &payloadBackLen);
    }

    /* 输出（BSP）：ASCII 英文，见 AGENTS.md 5.2 节 */
    BSP_print("sensor> #");
    BSP_printU32(me->seq);
    BSP_print(" ang=");
    printSignedFixed(angFilt[DRV_SENSOR_AXIS_X], 2U);
    BSP_print(",");
    printSignedFixed(angFilt[DRV_SENSOR_AXIS_Y], 2U);
    BSP_print(",");
    printSignedFixed(angFilt[DRV_SENSOR_AXIS_Z], 2U);
    BSP_print("deg t=");
    printSignedFixed(s.temp_cC, 2U);
    BSP_print("C frame=");
    BSP_printU32((uint32_t)frameLen);
    BSP_print("B selfcheck=");
    BSP_printLine(svcStr(sst));

    if ((sst == SVC_OK) && (cmdBack == SENSOR_CMD_REPORT) &&
        (payloadBackLen == sizeof(payload)) &&
        (payloadBack[0] == payload[0]) && (payloadBack[15] == payload[15])) {
        ++me->frames;
    }
    else {
        BSP_print("sensor> selfcheck mismatch: cmd=0x");
        BSP_printU32((uint32_t)cmdBack);
        BSP_print(" payloadLen=");
        BSP_printU32((uint32_t)payloadBackLen);
        BSP_printLine("");
    }
}

/*--------------------------------------------------------------------------*/
/* 状态处理函数                                                              */
/*--------------------------------------------------------------------------*/

/*..........................................................................*/
/** @brief Idle：等待启动命令。 */
QState Sensor_state_Idle(Sensor * const me, QEvt const * const e) {
    QState status;

    switch (e->sig) {
        case Q_ENTRY_SIG: {
            BSP_printLine("sensor> idle");
            status = Q_HANDLED();
            break;
        }
        case SYS_SENSOR_START_SIG: {
            status = Q_TRAN(&Sensor_state_Sampling);
            break;
        }
        case SYS_SENSOR_REPORT_SIG: {
            BSP_printLine("sensor> not sampling yet");
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/*..........................................................................*/
/** @brief Sampling：1 Hz 周期采样，20 ms 节拍推进驱动的非阻塞状态机。 */
QState Sensor_state_Sampling(Sensor * const me, QEvt const * const e) {
    QState status;

    switch (e->sig) {
        case Q_ENTRY_SIG: {
            me->seq = 0U;
            me->frames = 0U;
            me->errors = 0U;
            /* 首次 1 s 后触发，之后每 1 s 一次 */
            QTimeEvt_armX(&me->sampleEvt, BSP_TICKS_PER_SEC, BSP_TICKS_PER_SEC);
            /* 驱动推进节拍：每 20 ms 一次，与采样周期无关 */
            QTimeEvt_armX(&me->processEvt, SENSOR_PROCESS_TICKS, SENSOR_PROCESS_TICKS);
            BSP_printLine("sensor> sampling started (SCL3300 mode 3, 1 Hz)");
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            QTimeEvt_disarm(&me->sampleEvt);
            QTimeEvt_disarm(&me->processEvt);
            status = Q_HANDLED();
            break;
        }
        case SENSOR_PROCESS_SIG: {
            /* 推进 SCL3300 的非阻塞状态机；完成后立刻消费结果。
             * NOTE (为什么放在这里而不是 SENSOR_SAMPLE_SIG)：一次采样要 14 帧，
             *   必须靠高频节拍逐步推进，1 Hz 的事件推不动它。 */
            DRV_Sensor_Process();
            if (DRV_Sensor_IsDone()) {
                consumeSample(me);
            }
            status = Q_HANDLED();
            break;
        }
        case SENSOR_SAMPLE_SIG: {
            /* 1 Hz：发起一次新采样。器件未就绪（上电时序还没走完）时跳过本次。 */
            if (!DRV_Sensor_IsReady()) {
                BSP_printLine("sensor> device not ready yet");
            }
            else if (DRV_Sensor_StartSample() != DRV_OK) {
                /* 上一次还没结束：正常情况（不应发生），记为错误但不刷屏 */
                ++me->errors;
            }
            status = Q_HANDLED();
            break;
        }
        case SYS_SENSOR_REPORT_SIG: {
            /* 立即补采一次：先推进到位再发起，避免打断进行中的作业 */
            DRV_Sensor_Process();
            if (DRV_Sensor_IsDone()) {
                consumeSample(me);
            }
            (void)DRV_Sensor_StartSample();
            status = Q_HANDLED();
            break;
        }
        case SYS_SENSOR_STOP_SIG: {
            BSP_printLine("sensor> sampling stopped");
            status = Q_TRAN(&Sensor_state_Idle);
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/*..........................................................................*/
QState Sensor_initial(Sensor * const me, void const * const par) {
    (void)me;
    (void)par;

    return Q_TRAN(&Sensor_state_Idle);
}

/*..........................................................................*/
/**
* @brief 构造 AO_Sensor。
*/
void Sensor_ctor(void) {
    Sensor *me = (Sensor *)AO_Sensor;
    uint8_t axis;

    QActive_ctor(&me->super, Q_STATE_CAST(&Sensor_initial));
    QTimeEvt_ctorX(&me->sampleEvt, &me->super, SENSOR_SAMPLE_SIG, 0U);
    QTimeEvt_ctorX(&me->processEvt, &me->super, SENSOR_PROCESS_SIG, 0U);

    /* 滤波器状态由 AO 自己持有，初始化失败按 QP 惯例走断言 */
    for (axis = 0U; axis < (uint8_t)DRV_SENSOR_AXIS_COUNT; ++axis) {
        (void)SVC_Filter_MavgInit(&me->angFilt[axis], APP_CFG_SENSOR_MAVG_LEN);
        me->angLast_cdeg[axis] = 0;
        me->accLast_mg[axis] = 0;
    }

    me->seq         = 0U;
    me->frames      = 0U;
    me->errors      = 0U;
    me->tempLast_cC = 0;
    me->statusLast  = 0U;
}

/*..........................................................................*/
/**
* @brief 启动 AO_Sensor（队列存储随本模块走，BSP 不再参与装配）。
*
* NOTE (顺序)：先 DRV_Sensor_Init() 把驱动状态机置到起点，再由 20 ms 节拍
*   推进它的上电时序（SW Reset → 3 ms → 设模式 → 使能角度 → 自检）。
*/
void Sensor_start(void) {
    drv_status_t st;

    st = DRV_Sensor_Init();
    if (st != DRV_OK) {
        BSP_print("sensor> device init failed: ");
        BSP_printLine(drvStr(st));
    }

    QACTIVE_START(
        AO_Sensor,
        APP_SENSOR_PRIO,        /* 优先级来自 App 层装配表 */
        l_sensorQueueSto,       /* 队列存储属于 AO 自己     */
        Q_DIM(l_sensorQueueSto),
        (void *)0, 0U,          /* QV 内核不需要私有栈      */
        (QEvt *)0               /* 无初始化事件             */
    );
}
