/*============================================================================
* app_prio.h —— 活动对象（AO）优先级表
*
* 层次定位：App 层装配信息。优先级是「应用怎么组装」的信息，不是板级信息，
* 因此从 App/bsp/bsp.h 移到本文件（整改项 2，见 docs_sensor/07-接口约定.md）。
*
* 规则：
*   - 0 被 QP 保留（QF_MAX_ACTIVE 范围是 1..16），必须留空
*   - 优先级数值越小越优先（QP 语义），每个 AO 唯一
*   - 新增 AO 时在 APP_MAX_PRIO 之前追加一项，并同步更新
*     docs_sensor/04-任务调度.md#AO 一览
*===========================================================================*/
#ifndef APP_PRIO_H
#define APP_PRIO_H

enum AppActorPrio {
    APP_FORBIDDEN_PRIO = 0, /* 被 QP 保留，不要使用 */
    APP_NODE_PRIO,          /* AO_Node：调试控制台与 AT 解析 */
    APP_SENSOR_PRIO,        /* AO_Sensor：周期采样、滤波、上报 */
    APP_HEARTBEAT_PRIO,     /* AO_Heartbeat：周期心跳日志 */
    APP_BLE_PRIO,           /* AO_Ble：蓝牙配置、协议和上报 */
    APP_MODEM_PRIO,         /* AO_Modem：4G 入网、MQTT 与云业务 */
    APP_STORE_PRIO,         /* AO_Store：掉电参数存储（后台作业，最低优先级） */
    APP_SYSTEM_PRIO,        /* AO_System：系统级动作（延迟复位，只做检查）   */
    APP_MAX_PRIO
};

#endif /* APP_PRIO_H */
