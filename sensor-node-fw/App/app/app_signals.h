/*============================================================================
* app_signals.h —— 全局信号表（分段分配）
*
* 层次定位：App 层公共头文件，被各 AO 引用。
*
* 为什么集中定义：QP 的信号号在整个框架里是全局比较的。若每个 AO 各自从
* Q_USER_SIG 起编号，加到第二个 AO 就会撞号。因此所有信号集中在这里，按段
* 分配，每段 32 个：
*
*   全局段  0..31   跨 AO 投递的事件
*   Node 段 32..63  AO_Node 自用
*   Sensor 段 64..95 AO_Sensor 自用
*   Heartbeat 段 96..127
*   Ble 段 128..159
*   Modem 段 160..191
*   Store 段 192..223 AO_Store 自用（掉电参数存储）
*   System 段 224..255 AO_System 自用（系统级动作：延迟复位）
*
* 规则（见 docs_sensor/07-接口约定.md#信号与事件）：
*   - 段内信号只允许本 AO 自己使用；跨 AO 投递必须用全局段
*   - 新增 AO 时在表尾追加一段，_BASE 接上一段的 _END，不要往中间插
*   - 不再保留各 AO 头文件里的 MAX_SIG 计数项，段边界 *_SIG_END 即计数
*===========================================================================*/
#ifndef APP_SIGNALS_H
#define APP_SIGNALS_H

#include "qpc.h"

enum AppSignals {
    /* ---------- 全局段 0..31：跨 AO 投递 ---------- */
    GLOBAL_SIG_BASE = Q_USER_SIG,
    SYS_SENSOR_START_SIG = GLOBAL_SIG_BASE, /* App -> Sensor：启用驱动推进 */
    SYS_SENSOR_STOP_SIG,                    /* Node -> Sensor：停止采样     */
    SYS_SENSOR_REPORT_SIG,                  /* Node -> Sensor：立即上报一次 */
    SYS_SENSOR_PERIOD_SIG,                  /* 通信 AO -> Sensor：修改采样周期 */
    SYS_REBOOT_REQUEST_SIG,                 /* 任意命令来源 -> AO_System：请求系统复位
                                             * （串口/4G 都经 App_System_RequestReboot()
                                             *   投递；**只置请求，复位由 AO_System 延时执行**） */
    SYS_SENSOR_SAMPLE_CLOUD_SIG,            /* 4G -> Sensor：采集后用于云端上报。 */
    SYS_SENSOR_SAMPLE_BLE_SIG,              /* BLE -> Sensor：采集后用于蓝牙发送。 */
    SYS_BLE_ADVERT_REQUEST_SIG,             /* 按键业务 -> BLE：开启限时广播。 */
    SYS_MODEM_SIGNAL_REQUEST_SIG,           /* BLE -> Modem：刷新信号强度。 */
    SYS_REPORT_REQUEST_SIG,                 /* 所有业务触发 -> Modem：统一采集上报。 */
    SYS_BLE_SIGNAL_SIG,                     /* Modem -> BLE：信号强度快照。 */
    SYS_NODE_MEASUREMENT_SIG,               /* Sensor -> Node：寄存器测量快照。 */
    GLOBAL_SIG_END = GLOBAL_SIG_BASE + 32,

    /* ---------- AO_Node 段 32..63 ---------- */
    NODE_SIG_BASE = GLOBAL_SIG_END,
    NODE_HEARTBEAT_SIG = NODE_SIG_BASE,     /* 1 Hz 时间事件                */
    NODE_RX_BYTE_SIG,                       /* 调试串口收到字节             */
    NODE_SIG_END = NODE_SIG_BASE + 32,

    /* ---------- AO_Sensor 段 64..95 ---------- */
    SENSOR_SIG_BASE = NODE_SIG_END,
    SENSOR_SAMPLE_SIG = SENSOR_SIG_BASE,    /* 历史采样信号，当前不创建定时器            */
    SENSOR_PROCESS_SIG,                     /* 推进 SCL3300 的非阻塞状态机  */
    SENSOR_SIG_END = SENSOR_SIG_BASE + 32,

    /* ---------- AO_Heartbeat 段 96..127 ---------- */
    HEARTBEAT_SIG_BASE = SENSOR_SIG_END,
    HEARTBEAT_TICK_SIG = HEARTBEAT_SIG_BASE, /* 周期心跳时间事件（默认 2 s） */
    HEARTBEAT_SIG_END = HEARTBEAT_SIG_BASE + 32,
    /* ---------- AO_Ble 段 128..159 ---------- */
    BLE_SIG_BASE = HEARTBEAT_SIG_END,
    BLE_PROCESS_SIG = BLE_SIG_BASE,
    BLE_RX_SIG,
    BLE_MEASUREMENT_SIG,
    BLE_SIG_END = BLE_SIG_BASE + 32,

    /* ---------- AO_Modem 段 160..191 ---------- */
    MODEM_SIG_BASE = BLE_SIG_END,
    MODEM_PROCESS_SIG = MODEM_SIG_BASE,
    MODEM_RX_SIG,
    MODEM_MEASUREMENT_SIG,
    MODEM_SIG_END = MODEM_SIG_BASE + 32,

    /* ---------- AO_Store 段 192..223 ---------- */
    STORE_SIG_BASE = MODEM_SIG_END,
    STORE_TICK_SIG = STORE_SIG_BASE,   /* 10 ms 周期：推进写周期等待与扫描 */
    STORE_BUS_SIG,                     /* I2C 中断：一次总线传输结束        */
    STORE_CMD_SIG,                     /* 有新请求：参数被改 / 请求扫描     */
    STORE_SIG_END = STORE_SIG_BASE + 32,

    /* ---------- AO_System 段 224..255 ---------- */
    SYSTEM_SIG_BASE = STORE_SIG_END,
    SYSTEM_TICK_SIG = SYSTEM_SIG_BASE, /* 100 ms 周期：推进重启倒计时       */
    SYSTEM_SIG_END = SYSTEM_SIG_BASE + 32
};

#endif /* APP_SIGNALS_H */
