/*============================================================================
* app.c —— App 层装配实现
*
* 层次定位：App 层。这里只做装配，不写业务逻辑。
*
* 顺序要求：先构造（*_ctor）全部 AO，再启动（*_start）。构造只做对象初始化，
* 启动才把事件队列与优先级交给 QP 内核。
*===========================================================================*/
#include "app.h"
#include "app_config.h"
#include "app_signals.h"
#include "node.h"
#include "sensor.h"
#include "heartbeat.h"
#include "ble.h"
#include "modem.h"
#include "store.h"
#include "system.h"
#include "bsp.h"
/* 说明：本模块不展开任何 QP 断言宏（只用 QACTIVE_START，它在各 AO 的 _start()
* 里），因此不需要 Q_DEFINE_THIS_MODULE，见 AGENTS.md 第 5.2 节。
*/

/**
* @brief 构造并启动本应用的全部活动对象。
*/
void App_start(void) {
#if (APP_CFG_MODEM_ENABLE != 0) || (APP_CFG_BLE_ENABLE != 0)
    /* NOTE (单池)：QF 要求事件池按 evtSize **严格递增**注册（Qpc/platform/src/
    * qf_dyn.c:119 的 Q_REQUIRE_ID(201) 比较的是上一个池的实际块大小，不是"大于等于"），
    * 而 BleMeasurementEvt 与 ModemMeasurementEvt 的字段完全一致、尺寸相同，
    * 注册两个池必然命中断言 201。因此只注册一个池：Q_NEW 默认从 0 号池分配，
    * 两种事件共用它。池元素取两者的并集，以后任一事件变大也不用再动顺序。
    * 容量 16 = BLE 8 + Modem 8，两者各自最多同时持有 8 个快照。 */
    typedef union {
#if (APP_CFG_BLE_ENABLE != 0)
        BleMeasurementEvt ble;
#endif
#if (APP_CFG_MODEM_ENABLE != 0)
        ModemMeasurementEvt modem;
#endif
    } MeasurementEvtPool;
    static QF_MPOOL_EL(MeasurementEvtPool) measurementPool[16];
    QF_poolInit(measurementPool, sizeof(measurementPool), sizeof(MeasurementEvtPool));
#endif
    /* 接线：把「串口收到字节」的回调注册给 BSP。
    * 这样 BSP 不需要包含任何 App 头文件，也不需要认识 AO（依赖单向向下）。
    */
    BSP_uartRxSetSink(Node_uartRxIsrSink);
#if (APP_CFG_BLE_ENABLE != 0)
    BSP_bleUartSetSink(Ble_uartRxIsrSink);
#endif
#if (APP_CFG_MODEM_ENABLE != 0)
    BSP_modemUartSetSink(Modem_uartRxIsrSink);
#endif
    /* I2C 完成回调同理：中断里只投递事件，存储状态机由 AO_Store 推进。
    * 存储功能关闭时也注册（不会有人发起传输，回调不会被调用）。 */
    BSP_i2cSetSink(Store_i2cSink);

    /* 构造：只做成员与时间事件的初始化。
    * NOTE (顺序)：Store_ctor() 必须最先——它把参数影子填成编译期默认值，
    * 而 Ble_ctor() 会立刻用这些值构造 BleConfig（介质里的有效记录稍后由
    * AO_Store 异步读回来就地覆盖）。 */
    Store_ctor();
    Node_ctor();
    Sensor_ctor();
    Heartbeat_ctor();
    /* NOTE (AO_System)：只做"延迟复位"，构造顺序无关——它不读任何其它 AO 的数据，
     * 只在自己收到请求后才去调它们的**只读**查询。 */
    System_ctor();
#if (APP_CFG_BLE_ENABLE != 0)
    Ble_ctor();
#endif
#if (APP_CFG_MODEM_ENABLE != 0)
    Modem_ctor();
#endif

    /* 启动：队列存储由各 AO 自己的 _start() 持有并交给内核 */
    Store_start();
    Node_start();
    Sensor_start();
    Heartbeat_start();
    System_start();
#if (APP_CFG_BLE_ENABLE != 0)
    Ble_start();
#endif
#if (APP_CFG_MODEM_ENABLE != 0)
    Modem_start();
#endif

    /* 启动横幅尾部：AO 列表由 App 层提供（BSP 不认识 App）。
    * NOTE (维护)：新增/删除 AO 时在这里追加一段，并同步 docs_sensor/04-任务调度.md
    * 的「AO 一览」。分段打印（BSP_print 不带换行）是为了避免 #if 嵌套导致
    * 组合数爆炸——原先四个分支的写法每加一个 AO 就要翻倍。 */
    BSP_print("actors : node p1 | sensor p2 | heartbeat p3");
#if (APP_CFG_BLE_ENABLE != 0)
    BSP_print(" | ble p4");
#endif
#if (APP_CFG_MODEM_ENABLE != 0)
    BSP_print(" | modem p5");
#endif
#if (APP_CFG_STORE_ENABLE != 0)
    BSP_print(" | store p6");
#endif
    /* AO_System 恒在（没有开关宏）：它只管延迟复位，不占外设、不占总线。 */
    BSP_print(" | system p7");
    BSP_printLine("");
    BSP_printLine("boot ok");

    /* 采样任务上电即常开（1 Hz 自动上报）。
    * NOTE (设计)：调试串口只走 AT 指令，不再有 s/x/d 单字符命令，
    * 采样因此不需要外部触发，装配时直接送一次开始采样请求。
    */
    {
        static QEvt const sensorStartEvt = { (QSignal)SYS_SENSOR_START_SIG, 0U, 0U };
        (void)QACTIVE_POST_X(AO_Sensor, &sensorStartEvt, 0U, 0U);
    }
}
