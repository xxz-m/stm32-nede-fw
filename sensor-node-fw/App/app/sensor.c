/*============================================================================
* sensor.c —— 应用层：AO_Sensor 实现
*
* 层次定位：App 层。链路演示：App → Service（滤波 + 组帧）→ Driver（读器件）
*           → BSP（串口输出与时间基准）。
*
* 状态机：
*
*   Idle ---- SYS_SENSOR_START_SIG ----> Sampling
*   Idle <--- SYS_SENSOR_STOP_SIG ------ Sampling
*   两态都处理 SYS_SENSOR_REPORT_SIG（立即上报一次，不改变状态）
*
* 采样周期：1 Hz（BSP_TICKS_PER_SEC）
*===========================================================================*/
#include "sensor.h"
#include "app_config.h"
#include "app_signals.h"
#include "app_prio.h"
#include "bsp.h"
#include "drv_sensor.h"
#include "svc_frame.h"
#include "ble.h"
#include "modem.h"

/* 本模块在 QP 断言里的名字 */
Q_DEFINE_THIS_MODULE("sensor")

/* QP/C 断言钩子（实现在 App/bsp/bsp.c） */
void Q_onAssert(char const * const module, int loc);

/* 滑动平均窗口长度：1 Hz 采样下相当于 4 s平均 */
#define SENSOR_MAVG_LEN (4U)

/* AO 实例与事件队列（队列存储归 AO 自己所有，整改项 5） */
static Sensor l_sensor;
QActive * const AO_Sensor = &l_sensor.super;
static QEvt const *l_sensorQueueSto[8];

/*--------------------------------------------------------------------------*/
/* 本文件内部辅助函数                                                        */
/*--------------------------------------------------------------------------*/

/** @brief 打印 0.01 标度的定点数（例如 2344 -> "23.44"）。 */
static void print_cC(int32_t value) {
    int32_t ip;
    int32_t fp;

    if (value < 0) {
        BSP_print("-");
        value = -value;
    }
    ip = value / 100;
    fp = value % 100;
    BSP_printU32((uint32_t)ip);
    BSP_print(".");
    if (fp < 10) {
        BSP_print("0");
    }
    BSP_printU32((uint32_t)fp);
}

/** @brief 打印 0.1 标度的定点数（例如 456 -> "45.6"）。 */
static void print_permil(uint16_t value) {
    BSP_printU32((uint32_t)(value / 10U));
    BSP_print(".");
    BSP_printU32((uint32_t)(value % 10U));
}

/** @brief 把 Driver 状态码转成可读文本（只用于打印）。 */
static char const *drvStr(drv_status_t st) {
    switch (st) {
        case DRV_OK:          return "DRV_OK";
        case DRV_ERR_PARAM:   return "DRV_ERR_PARAM";
        case DRV_ERR_BUS:     return "DRV_ERR_BUS";
        case DRV_ERR_TIMEOUT: return "DRV_ERR_TIMEOUT";
        case DRV_ERR_NODEV:   return "DRV_ERR_NODEV";
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
        default:             return "SVC_ERR_UNKNOWN";
    }
}

/**
* @brief 采样一次：读器件 -> 滤波 -> 组帧 -> 解析自检 -> 打印。
*
* 这是本工程里「四层协作」的最小完整样例：
*   DRV_Sensor_*  （Driver：器件语义）
*   SVC_Filter_*  （Service：无状态纯函数 + 调用方持有状态）
*   SVC_Frame_*   （Service：组帧与校验）
*   BSP_print*    （BSP：唯一的输出通道）
*/
static void sampleOnce(Sensor * const me) {
    int32_t  tempRaw = 0;
    uint16_t rhRaw   = 0;
    int32_t  tempMed;
    int32_t  tempFilt;
    int32_t  rhFilt;
    uint8_t  payload[6];
    uint8_t  frame[SVC_FRAME_MAX_SIZE];
    size_t   frameLen = 0U;
    uint8_t  cmdBack = 0U;
    uint8_t  payloadBack[6];
    size_t   payloadBackLen = 0U;
    drv_status_t dst;
    svc_status_t sst;
    uint32_t uiT;
    uint16_t uiRh;

    /* 1) 读器件（Driver） */
    dst = DRV_Sensor_ReadTemp_cC(&tempRaw);
    if (dst != DRV_OK) {
        BSP_print("sensor> sample failed: ");
        BSP_printLine(drvStr(dst));
        return;
    }
    dst = DRV_Sensor_ReadRh_permil(&rhRaw);
    if (dst != DRV_OK) {
        BSP_print("sensor> sample failed: ");
        BSP_printLine(drvStr(dst));
        return;
    }

    /* 2) 滤波（Service）：三点中值去尖峰 + 滑动平均平滑 */
    tempMed  = SVC_Filter_Median3(tempRaw, me->tempLast_cC, tempRaw);
    tempFilt = SVC_Filter_MavgPush(&me->tempFilt, tempMed);
    rhFilt   = SVC_Filter_MavgPush(&me->rhFilt, (int32_t)rhRaw);

    me->tempLast_cC   = (int32_t)tempFilt;
    me->rhLast_permil = (uint16_t)rhFilt;
    ++me->seq;

#if (APP_CFG_BLE_ENABLE != 0)
    /* 采样值以动态事件快照交给 AO_Ble，双方不共享可变对象。 */
    {
        BleMeasurementEvt *snapshot = Q_NEW(BleMeasurementEvt, BLE_MEASUREMENT_SIG);
        snapshot->temperature_cC = tempFilt;
        snapshot->humidity_permil = (uint16_t)rhFilt;
        QACTIVE_POST(AO_Ble, &snapshot->super, me);
    }
#endif
#if (APP_CFG_MODEM_ENABLE != 0)
    {
        ModemMeasurementEvt *snapshot = Q_NEW(ModemMeasurementEvt, MODEM_MEASUREMENT_SIG);
        snapshot->temperature_cC = tempFilt;
        snapshot->humidity_permil = (uint16_t)rhFilt;
        QACTIVE_POST(AO_Modem, &snapshot->super, me);
    }
#endif

    /* 3) 组帧（Service）：载荷 = 温度 int32 小端 + 湿度 uint16 小端 */
    uiT  = (uint32_t)tempFilt;
    uiRh = (uint16_t)rhFilt;
    payload[0] = (uint8_t)(uiT & 0xFFU);
    payload[1] = (uint8_t)((uiT >> 8) & 0xFFU);
    payload[2] = (uint8_t)((uiT >> 16) & 0xFFU);
    payload[3] = (uint8_t)((uiT >> 24) & 0xFFU);
    payload[4] = (uint8_t)(uiRh & 0xFFU);
    payload[5] = (uint8_t)((uiRh >> 8) & 0xFFU);

    sst = SVC_Frame_Pack(SENSOR_CMD_REPORT, payload, sizeof(payload),
                         frame, sizeof(frame), &frameLen);

    /* 4) 立即解析回来做自检（无硬件环境下唯一能验证链路正确性的手段） */
    if (sst == SVC_OK) {
        sst = SVC_Frame_Parse(frame, frameLen, &cmdBack,
                              payloadBack, sizeof(payloadBack), &payloadBackLen);
    }

    /* 5) 输出（BSP） */
    BSP_print("sensor> #");
    BSP_printU32(me->seq);
    BSP_print(" t=");
    print_cC(tempFilt);
    BSP_print("C rh=");
    print_permil((uint16_t)rhFilt);
    BSP_print("% frame=");
    BSP_printU32((uint32_t)frameLen);
    BSP_print("B selfcheck=");
    BSP_printLine(svcStr(sst));

    if ((sst == SVC_OK) && (cmdBack == SENSOR_CMD_REPORT) &&
        (payloadBackLen == sizeof(payload)) &&
        (payloadBack[0] == payload[0]) && (payloadBack[5] == payload[5])) {
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
/** @brief Sampling：1 Hz 周期采样。 */
QState Sensor_state_Sampling(Sensor * const me, QEvt const * const e) {
    QState status;

    switch (e->sig) {
        case Q_ENTRY_SIG: {
            me->seq = 0U;
            /* 首次 1 s后触发，之后每 1 s一次 */
            QTimeEvt_armX(&me->sampleEvt, BSP_TICKS_PER_SEC, BSP_TICKS_PER_SEC);
            BSP_printLine("sensor> sampling started (1 Hz)");
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            QTimeEvt_disarm(&me->sampleEvt);
            status = Q_HANDLED();
            break;
        }
        case SENSOR_SAMPLE_SIG: {
            sampleOnce(me);
            status = Q_HANDLED();
            break;
        }
        case SYS_SENSOR_REPORT_SIG: {
            sampleOnce(me); /* 立即补采一次并上报 */
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

    QActive_ctor(&me->super, Q_STATE_CAST(&Sensor_initial));
    QTimeEvt_ctorX(&me->sampleEvt, &me->super, SENSOR_SAMPLE_SIG, 0U);

    /* 滤波器状态由 AO 自己持有，初始化失败按 QP 惯例走断言 */
    (void)SVC_Filter_MavgInit(&me->tempFilt, SENSOR_MAVG_LEN);
    (void)SVC_Filter_MavgInit(&me->rhFilt, SENSOR_MAVG_LEN);

    me->seq           = 0U;
    me->frames        = 0U;
    me->tempLast_cC   = 0;
    me->rhLast_permil = 0U;
}

/*..........................................................................*/
/**
* @brief 启动 AO_Sensor（队列存储随本模块走，BSP 不再参与装配）。
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
