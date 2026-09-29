/*============================================================================
* app_config.h —— 全局配置（宏）
*
* 层次定位：**配置头，工程内所有层（App / Service / Driver / BSP）都可以依赖**。
*           （AGENTS.md 第 5.1 节的例外条款，见该节说明）
*
* 使用规则：
*   - 本文件只放「配置宏」，不放类型、不声明函数、不 include 其他项目头文件，
*     以免形成依赖环
*   - 设备能力、功能开关、缓冲尺寸、周期等一律在这里集中管理；其它层不得
*     再散落硬编码常量
*   - 关闭某个功能时，对应的调用点在编译期被消除（例如 LOG 关闭后
*     `LOG_I(...)` 展开为 `((void)0)`），调用点**不需要写 if**
*   - 打开/关闭后请重新编译并复核 docs_sensor/03-资源分配.md 的占用
*===========================================================================*/
#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* 设备与固件标识 -----------------------------------------------------------*/
#define APP_CFG_FW_NAME             "sensor-node-fw"
#define APP_CFG_FW_VERSION          "0.3.0"
#define APP_CFG_BOARD_NAME          "STM32F103RCT6"
/*! 构建时间戳：由编译器自动填入（__DATE__ __TIME__），启动横幅会打印 */
#define APP_CFG_FW_BUILD_DATE       __DATE__ " " __TIME__

/* 日志等级（数值越大越详细） -----------------------------------------------*/
#define APP_CFG_LOG_LVL_NONE        (0)
#define APP_CFG_LOG_LVL_ERROR       (1)
#define APP_CFG_LOG_LVL_WARN        (2)
#define APP_CFG_LOG_LVL_INFO        (3)
#define APP_CFG_LOG_LVL_DEBUG       (4)

/* 调试日志：总开关 + 等级 --------------------------------------------------*/
/*! 1 = 启用日志（异步发送），0 = 全部日志调用在编译期消失 */
#define APP_CFG_LOG_ENABLE          (1)
/*! 只输出 <= 该等级的日志；关掉某个等级同样在编译期消失 */
#define APP_CFG_LOG_LEVEL           (APP_CFG_LOG_LVL_DEBUG)

/*! 异步发送环形队列大小（字节），拥有者是 BSP。
*  NOTE (2026-09-27 由 256 提到 512)：日志缓冲提到 200 之后，最多可能出现约 190 字符的
*  单条日志（4G 连接诊断那行）。环太小会把一整行拆掉一半、看起来像"固件在截断"，
*  所以至少要让**一条最长的日志装得下**，并留一点余量。 */
#define APP_CFG_LOG_TX_RING_SIZE    (512U)
/*! 单条日志格式化缓冲大小（字节）。
*  NOTE (2026-09-27 由 96 提到 200，一次真实的教训)：这个缓冲是 `SVC_Log_Printf()` 里的
*  栈数组，`vsnprintf` 会**静默截断**超长内容。而 `App/component/modem` 的连接诊断行
*  （`step k/N <tag> cmd="…" rx="…" reason=n`）最长可达约 168 字符——它由组件的
*  `MODEM_DIAG_LINE_MAX` 决定。两者口径不一致的后果：上板日志在 95 字符处被砍，
*  **恰好丢掉 `reason=` 与 CME 错误码的最后一位**，排查时看到的是一行"没有结论的结论"。
*  改成 ≥ 168 + 前缀（"4g-core: " 9 + "[D 4294967295] " 约 15）的 200 之后，
*  组件拼多少就能打多少。**改这个值时请一并核对 `MODEM_DIAG_LINE_MAX`。** */
#define APP_CFG_LOG_FMT_BUF_SIZE    (200U)
/*! 队列满时：1 = 丢弃新日志（不阻塞），0 = 覆盖最旧日志 */
#define APP_CFG_LOG_DROP_WHEN_FULL  (1)

/* AT 指令（调试串口） ------------------------------------------------------*/
/*! 1 = 解析 AT 指令（标准 AT 形态，命令表见 App/service/svc_at.h），
*  0 = 丢弃接收内容（svc_at.c 整体编译成空操作，调用点不变）。 */
#define APP_CFG_AT_ENABLE           (1U)
/*! 单行 AT 命令的最大长度（含结尾的 '\0' 位置）。
*  NOTE (2026-09-27 由 24 提到 64)：参数设置指令形如
*  `AT+UUID="CDAB0000-1234-5678-1234-56789ABCDEF0"`（8 + 36 + 1 = 45 字符），
*  原来的 24 会直接丢弃整行，表现为「指令毫无反应」。 */
#define APP_CFG_AT_LINE_MAX         (64U)

/* 参数存储（I2C EEPROM） ---------------------------------------------------*/
/*! 1 = 启用掉电参数存储（I2C EEPROM + AT24C04 驱动 + AO_Store）；
*  0 = 整体关闭总线访问，参数退化为「只有编译期默认值」（影子仍在，各层调用点不变）。 */
#define APP_CFG_STORE_ENABLE            (1U)
/*! I2C 从机 7 位地址。AT24C04 的 A1/A2 全部接地时为 0x50（用户 2026-09-27 确认）。
*  换接线只改这一行；地址不确定时用调试口的 `AT+I2C?` 扫描确认。 */
#define APP_CFG_STORE_I2C_ADDR          (0x50U)
/*! 器件总容量（字节）：AT24C04 = 4 Kbit = 512 B。 */
#define APP_CFG_STORE_CAPACITY          (512U)
/*! 页写大小（字节）：AT24C04 = 16 B；写不能跨页，否则会回卷覆盖页首。 */
#define APP_CFG_STORE_PAGE_SIZE         (16U)
/*! EEPROM 内部写周期 tWR（毫秒）。AT24C04 数据手册 max 5 ms，这里留 6 ms 余量。
*  这一页写完必须等它结束才能碰下一段，否则从机不应答。 */
#define APP_CFG_STORE_WRITE_CYCLE_MS    (6U)
/*! 单次中断传输的超时（毫秒）。100 kHz 下一段最多 16 B（约 1.5 ms），
*  50 ms 已经非常宽松；超时说明外设/中断出了问题，不是「慢」。 */
#define APP_CFG_STORE_XFER_TIMEOUT_MS   (50U)
/*! 一段传输失败的重试次数；用尽后再做一次总线恢复，仍失败才判作业失败。 */
#define APP_CFG_STORE_RETRY             (3U)
/*! 存储状态机的节拍周期（毫秒）。页与页之间靠它推进写周期等待。 */
#define APP_CFG_STORE_PROCESS_PERIOD_MS (10U)
/*! A/B 双槽的起始地址：AT24C04 每 256 B 一个块（第 9 位地址由器件地址携带），
*  两槽各占一个块，互不重叠，也不会跨块。记录最大长度必须小于槽间距。 */
#define APP_CFG_STORE_SLOT_A_ADDR       (0U)
#define APP_CFG_STORE_SLOT_B_ADDR       (256U)
/*! I2C 扫描时单次探测的超时（毫秒）。只给 `AT+I2C?` 诊断用，越小越不占 AO。 */
#define APP_CFG_STORE_PROBE_TIMEOUT_MS  (2U)
/*! I2C 扫描的地址范围（7 位地址，含端点）。 */
#define APP_CFG_STORE_SCAN_FIRST        (0x08U)
#define APP_CFG_STORE_SCAN_LAST         (0x77U)

/* 心跳任务 -----------------------------------------------------------------*/
#define APP_CFG_HEARTBEAT_ENABLE    (1)
/*! 心跳周期（秒） */
#define APP_CFG_HEARTBEAT_PERIOD_S  (2U)

/* 采样任务 -----------------------------------------------------------------*/
#define APP_CFG_SENSOR_ENABLE       (1)
/*! 采样周期（秒） */
#define APP_CFG_SENSOR_PERIOD_S     (1U)
/*! 滑动平均窗口长度 */
#define APP_CFG_SENSOR_MAVG_LEN     (4U)

/* 系统动作：延迟复位（AO_System）-------------------------------------------*/
/*! 重启倒计时的检查周期（毫秒）。每拍只做几次只读判断，代价极低。 */
#define APP_CFG_SYSTEM_PROCESS_PERIOD_MS    (100U)
/*! 复位前的**最短**延时（毫秒）：给「命令回复出设备」留一个下限。
*  串口的回复走异步发送环、4G 的回复要等 `AT+MQTTPUB` 与模组应答；
*  这一段保证即使链路空闲判据一时看不出来，回复也有机会先出去。 */
#define APP_CFG_SYSTEM_REBOOT_MIN_DELAY_MS  (500U)
/*! 复位前的**硬上限**（毫秒）：到点无条件复位。
*  NOTE (为什么必须有)：若 EEPROM 坏了导致 dirty 永远清不掉，或模组一直占着上行，
*    「等条件满足」就退化成「永不重启」——一条 `AT+REBOOT` 能把设备卡到只能人工断电。
*    5 s > 单条 AT 命令超时（3 s）+ 一次退避，足够回复出完。 */
#define APP_CFG_SYSTEM_REBOOT_MAX_WAIT_MS   (5000U)

/* 串口接收 -----------------------------------------------------------------*/
/*! 接收环形缓冲大小（字节），拥有者是 BSP */
#define APP_CFG_UART_RX_RING_SIZE   (64U)

/* BLE 通信组件 -------------------------------------------------------------*/
/*! 功能与芯片后端；更换芯片时只切换 BACKEND 并加入对应 backend 源文件。 */
#define APP_CFG_BLE_ENABLE                    (1)
#define APP_CFG_BLE_BACKEND_VG6328            (1U)
#define APP_CFG_BLE_BACKEND                   (APP_CFG_BLE_BACKEND_VG6328)
/*! 组件内部诊断开关；仍受 APP_CFG_LOG_ENABLE/LEVEL 二次过滤。 */
#define APP_CFG_BLE_TRACE_ENABLE              (1U)
/*! 1 = 把**模组发来的每一行**原样打进日志（含 `+CONNECT-` / `+DISCONNECT-` 等通知）。
*
*  NOTE (为什么需要它，2026-09-28)：与 4G 侧的 `APP_CFG_MODEM_TRACE_URC` 同款用途。
*    BLE 侧此前没有这个开关，而 `BLE_Protocol_Line()` 对认不出的行是**静默丢弃**的，
*    于是现场无法区分这两种完全不同的情况：
*      ① 手机压根没连上（模组一个字都没说）；
*      ② 连上了、模组也说话了，但那条通知我们不认识。
*    用户 2026-09-28 报的「扫描能看到、连不上、串口也没有任何日志」正需要它来分辨。
*  NOTE (凭据纪律)：含 `{` 的行（小程序的 JSON 载荷）**一律不打**——auth 请求里带 PIN。
*    模组自己的 AT 行不含凭据，可以整行打。
*  NOTE (排障完请改回 0)：它会持续刷屏（连接期每一步模组应答都打一行）。 */
#define APP_CFG_BLE_TRACE_RX                  (1U)

/*! 占用哪一路 USART（1/2/3 = USART1/2/3，与 BSP 的 bsp_uart_id_t 取值一致）。
*  NOTE (2026-09-20 对调)：BLE 原先占 USART3，按实机接线改为 **USART2（PA2/PA3）**；
*  4G 模组相应移到 USART3（PB10/PB11）。换接线只改这一行与 APP_CFG_MODEM_UART_ID，
*  端口的 HAL 句柄由 BSP 按编号解析，不需要再改 BSP 代码。 */
#define APP_CFG_BLE_UART_ID                   (2U)

/*! 平台 UART 与 BSP 环形缓冲。 */
#define APP_CFG_BLE_UART_BAUD                 (115200U)
#define APP_CFG_BLE_RX_RING_SIZE              (512U)
#define APP_CFG_BLE_TX_RING_SIZE              (1536U)

/*! 协议容量；FRAME_MAX=20、分片数据=14 是当前小程序协议约束。 */
#define APP_CFG_BLE_JSON_MAX                  (768U)
#define APP_CFG_BLE_MAX_PARAMS                (8U)
#define APP_CFG_BLE_TX_DEPTH                  (2U)
#define APP_CFG_BLE_FRAME_MAX                 (20U)
#define APP_CFG_BLE_FRAGMENT_DATA_MAX         (14U)
#define APP_CFG_BLE_RX_BUDGET                 (64U)

/*! 非阻塞状态机时间参数。 */
#define APP_CFG_BLE_PROCESS_PERIOD_MS         (100U)
#define APP_CFG_BLE_STARTUP_DELAY_MS          (500U)
#define APP_CFG_BLE_COMMAND_TIMEOUT_MS        (1000U)
#define APP_CFG_BLE_PROBE_ATTEMPTS            (3U)
#define APP_CFG_BLE_PROBE_RETRY_MS            (2000U)
#define APP_CFG_BLE_RESTART_WAIT_MS           (500U)
#define APP_CFG_BLE_RESTART_PROBE_ATTEMPTS    (6U)
#define APP_CFG_BLE_REPORT_PERIOD_S           (60U)

/*! 产品身份与小程序配置；通用组件内不得写死这些值。
*  NOTE (2026-09-28 删掉 `APP_CFG_BLE_NAME`)：产品决定**蓝牙广播名就是 SN**，于是宏与
*   参数槽位都不再需要「独立的广播名」（见 svc_param.h 的退役槽位 NOTE）。原来的
*   `APP_CFG_BLE_NAME` 与 `APP_CFG_BLE_SN` 默认值恰好相同，留着它反而会被误认为
*   「改这个宏就能改广播名」，而实际广播名取 `SVC_PARAM_BLE_SN`。 */
#define APP_CFG_BLE_SN                        "VWA2608A0013"
#define APP_CFG_BLE_PIN                       "062500"
#define APP_CFG_BLE_SERVICE_UUID              "CDAB0000-1234-5678-1234-56789ABCDEF0"
#define APP_CFG_BLE_DEVICE_NAME               "VWA-0013"
#define APP_CFG_BLE_MODEL_CODE                "VWA-LB01"
#define APP_CFG_BLE_BATCH_CODE                "2026Q3-A01"

/* 4G Modem 通信组件 --------------------------------------------------------*/
/*! 功能与芯片后端；更换芯片时只切换 BACKEND 并加入对应 backend 源文件。
*  三个后端编译期三选一：未选中的用 #if 排除，既不产生代码也不占 Flash。 */
/*! NOTE (2026-09-20 开回来)：4G 模组（**MN316**）已按实机接线接到 `USART3`（PB10/PB11），
*  并新增电源使能 `MN316_EN`(PB9，**高开启**）——由 BSP_modemUartInit() 配成推挽输出并拉高，
*  与 BLE 的 `VG6_EN`(PB1) 同形。复位脚 `NB_RESET`(PB8) 按用户决定**不配置**（保持悬空）。
*  关掉本宏时整块 4G 在编译期消失（组件、BSP 端口、事件池与采样快照投递都带本守卫）。 */
#define APP_CFG_MODEM_ENABLE                  (1)
#define APP_CFG_MODEM_BACKEND_MN316           (1U)
#define APP_CFG_MODEM_BACKEND_NT26            (2U)
#define APP_CFG_MODEM_BACKEND_ML307C          (3U)
/*! 当前选用的 4G 后端：**编译期三选一，只改这一行**。
*  NOTE (2026-09-19 改)：这里原先是「当前选用 ML307C-DTU（度云 Cat.1 DTU，方言与 NT26 同族
*   但有关键差异）」的硬描述——换后端之后它就变成错的、反而误导；显示名现在由下面推导，
*   所以「当前选谁」只由这一行决定，无需再用注释说明。 */
/*! NOTE (2026-09-20 换后端)：实机接的是 **MN316**（AT 口 9600，2026-09-19 实机扫描确认），
*  故由 ML307C 切到 MN316；显示名会自动跟着变成 "MN316"，不用再手改任何宏。
*  换回 ML307C / NT26 同样只改这一行。 */
#define APP_CFG_MODEM_BACKEND                 (APP_CFG_MODEM_BACKEND_MN316)
/*! 后端显示名，只用于启动日志。
*  NOTE (由 BACKEND 推导，2026-09-19 改)：这里原先是第二个必须手工同步的宏，漏改就会出现
*  「镜像里跑的是 MN316、启动日志却写 ML307C」——这类错觉最难查（现场会先怀疑接线）。
*  现在由 APP_CFG_MODEM_BACKEND 推导，**换后端只需改上面一行**；取值写错由下面的
*  #else 在编译期直接报错，不再需要靠注释提醒。 */
#if (APP_CFG_MODEM_BACKEND == APP_CFG_MODEM_BACKEND_MN316)
#define APP_CFG_MODEM_BACKEND_NAME            "MN316"
#elif (APP_CFG_MODEM_BACKEND == APP_CFG_MODEM_BACKEND_NT26)
#define APP_CFG_MODEM_BACKEND_NAME            "NT26"
#elif (APP_CFG_MODEM_BACKEND == APP_CFG_MODEM_BACKEND_ML307C)
#define APP_CFG_MODEM_BACKEND_NAME            "ML307C"
#else
#error "APP_CFG_MODEM_BACKEND 取值无效：只能是 APP_CFG_MODEM_BACKEND_MN316 / NT26 / ML307C"
#endif
/*! 组件内部诊断开关；仍受 APP_CFG_LOG_ENABLE/LEVEL 二次过滤。 */
#define APP_CFG_MODEM_TRACE_ENABLE            (1U)
/*! 临时诊断（2026-09-27）：把模组**发来**的 `+MQTT*` 行原样打进日志。
*  用途：确认 MN316 下行通知的真实名称与主题字符串——组件对认不出的行是**静默丢弃**的，
*  所以「日志里什么都没有」并不能证明消息没到模组，必须把原始行打出来才能判断。
*  NOTE (用完请改回 0)：这是排障开关，下行通道确认打通后关掉，否则连接期会持续刷屏。 */
#define APP_CFG_MODEM_TRACE_URC               (1U)

/*! 平台 UART 与 BSP 环形缓冲（尺寸须为 2 的幂，BSP 用掩码取模）。 */
/*! 占用哪一路 USART（1/2/3 = USART1/2/3）；与 APP_CFG_BLE_UART_ID 一起构成装配信息。 */
#define APP_CFG_MODEM_UART_ID                  (3U)
/*! 模组串口波特率（现为 USART3 = PB10/PB11）。
*  NOTE (按后端分档，2026-09-19 改)：这里原先是**全局唯一**值，换模组必须改它，
*  而两边波特率不一致时串口**完全静默**——模组收到的是乱码、不会应答，我们这边也收不到
*  任何字节（`uart_err` 还会是 0），是最难定位的一类故障。改成按后端各给一档后，
*  换回去不会忘，也不必再动 ML307C 的实测值。
*  取值依据：
*    - ML307C-DTU：2026-09-17 实机实测 115200（见 ml307c-dtu-test-report）。
*    - MN316：按模组资料常用 **9600**（2026-09-19 按用户提供的信息设定，**待实测确认**）。
*      若实测为 115200，只改这一行即可，不影响其它后端。
*    - NT26：未实测，沿用 115200。
*  8N1 不变（三个模组一致）。改完重编重烧，启动横幅会打出实际生效值。 */
#if (APP_CFG_MODEM_BACKEND == APP_CFG_MODEM_BACKEND_MN316)
#define APP_CFG_MODEM_UART_BAUD                (9600U)
#else
#define APP_CFG_MODEM_UART_BAUD                (115200U)
#endif
#define APP_CFG_MODEM_RX_RING_SIZE             (512U)
#define APP_CFG_MODEM_TX_RING_SIZE             (1024U)

/*! 协议容量；命令缓冲要能放下 MN316 的十六进制负载，故约为负载的两倍。 */
#define APP_CFG_MODEM_COMMAND_SIZE             (768U)
#define APP_CFG_MODEM_RESPONSE_SIZE            (512U)
#define APP_CFG_MODEM_PAYLOAD_SIZE             (384U)
#define APP_CFG_MODEM_URC_SIZE                 (512U)
#define APP_CFG_MODEM_LINE_SIZE                (512U)
#define APP_CFG_MODEM_TOPIC_SIZE               (128U)
#define APP_CFG_MODEM_CELL_FIELD_SIZE          (12U)
#define APP_CFG_MODEM_TOKEN_SIZE               (24U)

/*! MQTT 主题前导 `/`：**一条主题一个宏**，取值只能是 `""` 或 `"/"`。 --------------------
*
*  NOTE (为什么逐条给，而不是按收发方向，2026-09-28 订正)：平台侧有**两个互不相干的主题
*   空间**，'/' 的归属由**主题用途**决定，与收/发方向无关：
*     - **数据上报空间**：属性上报页原文要求设备发布到
*       `/{productId}/{deviceId}/properties/report`（**带** '/'）——给平台画卡片与折线用；
*     - **命令/属性交互空间**：平台的「AT 控制主题模板」`{productId}/{deviceId}/command` 与
*       「回复主题模板」`{productId}/{deviceId}/command/reply`（**都不带** '/'）——命令与它的
*       回复在**同一格**里闭环。
*   把它当成"发布方向统一带 '/'"就会给 `command/reply` 多加一个 `/`：`#58`→`#62` 的反复就是
*   这么来的，实测症状是平台上停在 `MQTT_PUBLISHED · WAITING_RESPONSE`、**20 s 判"指令超时"**
*   （见 [[08-改动日志]] `#64`）。
*
*  NOTE (怎么改)：**平台单独要求哪一条换形态，就改哪一行**，不必碰 `modem_component.c`——
*   组件里的 `kTopicPrefix[]` 表与这 10 个宏**一一对应**（顺序 = 主题枚举顺序），并有编译期
*   护栏：每个宏的串长必须 ≤ 1 字符，写成别的直接编译报错。
*   拼法：`前缀 + <productId>/<deviceId> + 后缀`；后缀在 `kTopicSuffix[]`（自带 device 之后那个 `/`）。 */
#define APP_CFG_MODEM_TOPIC_PREFIX_REPORT         "/"  /* → /<pid>/<did>/properties/report  数据上报 */
#define APP_CFG_MODEM_TOPIC_PREFIX_READ           "/"   /* → <pid>/<did>/properties/read     读属性   */
#define APP_CFG_MODEM_TOPIC_PREFIX_READ_REPLY     "/"   /* → <pid>/<did>/properties/read/reply       */
#define APP_CFG_MODEM_TOPIC_PREFIX_WRITE          "/"   /* → <pid>/<did>/properties/write    写属性   */
#define APP_CFG_MODEM_TOPIC_PREFIX_WRITE_REPLY    "/"   /* → <pid>/<did>/properties/write/reply      */
#define APP_CFG_MODEM_TOPIC_PREFIX_INVOKE         "/"   /* → <pid>/<did>/function/invoke     功能调用 */
#define APP_CFG_MODEM_TOPIC_PREFIX_STATE          "/"   /* → <pid>/<did>/state-check         状态查询 */
#define APP_CFG_MODEM_TOPIC_PREFIX_STATE_REPLY    "/"   /* → <pid>/<did>/state-check/reply           */
#define APP_CFG_MODEM_TOPIC_PREFIX_COMMAND        "/"   /* → <pid>/<did>/command             AT 命令  */
#define APP_CFG_MODEM_TOPIC_PREFIX_COMMAND_REPLY  "/"   /* → <pid>/<did>/command/reply       AT 回复  */
/*! 云端 AT 命令下行（平台规则 3.2 的 `command` 主题）的字段缓冲。
*
*  NOTE (command 为什么不是平台允许的 256)：本设备 AT 行的上限是 `APP_CFG_AT_LINE_MAX`（64），
*  超长命令永远解析不出来，先存下来再丢只是白占 RAM。超限时直接回一条失败应答。
*
*  NOTE (id 取 64 的实测依据，2026-09-27)：平台真实下发的是 `cmd-at-<uuid>`，
*  例如 `cmd-at-d92b9f55-f96e-48ee-8fd1-dc3209db790e` —— **43 个字符**。
*  原定 48 只剩 4 字节余量，平台换个前缀就会静默变成「取不到 id → 回空 id 的失败应答」，
*  因此放大到 64，与 `APP_COMMAND_CORRELATION_MAX` 取齐（代价仅 16 字节 bss）。 */
#define APP_CFG_MODEM_DOWNLINK_ID_SIZE         (64U)
#define APP_CFG_MODEM_DOWNLINK_COMMAND_SIZE    (96U)
/*! 单次 Process 最多读取的串口字节数（防止一次事件里长时间占着 QV）。 */
#define APP_CFG_MODEM_RX_BUDGET                (64U)

/*! 非阻塞状态机时间参数；连接计划表里的每一步都从这些宏取值，不写裸数字。 */
#define APP_CFG_MODEM_PROCESS_PERIOD_MS        (20U)
/*! 单条命令的默认应答超时。 */
#define APP_CFG_MODEM_COMMAND_TIMEOUT_MS       (3000U)
/*! 上电后等模组启动稳定再探测。 */
#define APP_CFG_MODEM_STARTUP_DELAY_MS         (500U)
/*! AT 探活的单次应答超时与重发间隔、最少尝试次数。 */
#define APP_CFG_MODEM_PROBE_TIMEOUT_MS         (1500U)
#define APP_CFG_MODEM_AT_RETRY_MS              (500U)
#define APP_CFG_MODEM_PROBE_ATTEMPTS           (5U)
/*! 等待型步骤（等 SIM / 等附着 / 等通道）的重发间隔。 */
#define APP_CFG_MODEM_POLL_INTERVAL_MS         (1000U)
/*! 等网络注册的重发间隔（注册本身以秒级推进，轮询太密没有意义）。 */
#define APP_CFG_MODEM_REG_POLL_MS              (2000U)
/*! 各阶段的等待预算：超过就判失败，交回 AO 决定重连。 */
#define APP_CFG_MODEM_SIM_WAIT_MS              (30000U)
#define APP_CFG_MODEM_REGISTRATION_TIMEOUT_MS  (300000U)
#define APP_CFG_MODEM_ATTACH_TIMEOUT_MS        (30000U)
/*! 配置类命令（DTUTASK / MQTT 参数 / 主题登记）的应答超时。 */
#define APP_CFG_MODEM_CONFIG_TIMEOUT_MS        (10000U)
/*! NT26 通道上线与模组重启后的等待预算。 */
#define APP_CFG_MODEM_CHANNEL_TIMEOUT_MS       (60000U)
#define APP_CFG_MODEM_REBOOT_SETTLE_MS         (20000U)
/*! MN316 打开 MQTT / 等连接状态就绪 / 等订阅确认的超时。 */
#define APP_CFG_MODEM_MQTT_OPEN_TIMEOUT_MS     (60000U)
/*! MQTT 状态查询（`AT+MQTTSTAT?`）的总等待预算。
*  NOTE (2026-09-27)：手册（MN316 MQTT 用户手册 §2.3，第 12 页）里状态 3/4/6/7 分别是
*  「发送 Connect 包等 ack」「正在重连服务器」「建立 TCP 连接中」「TCP 连接已建立」——
*  这些**都是连接正在路上**，不是失败。本宏是「等它走完」的时间上限：到期仍未到 5，
*  才放弃等待、按未连接处理。 */
#define APP_CFG_MODEM_MQTT_STATE_TIMEOUT_MS    (30000U)
/*! 状态查询的重问间隔；太小会刷屏，太大会错过状态跃迁。 */
#define APP_CFG_MODEM_MQTT_STATE_POLL_MS       (2000U)
#define APP_CFG_MODEM_TOPIC_TIMEOUT_MS         (20000U)
/*! MQTTCFG 成功后给模组内部资源释放与参数生效的非阻塞稳定等待。 */
#define APP_CFG_MODEM_MQTT_OPEN_SETTLE_MS      (2000U)
/*! MQTTOPEN 明确失败后的退避重试间隔，避免每个 Process 周期立即重发。 */
#define APP_CFG_MODEM_MQTT_OPEN_RETRY_MS       (5000U)
/*! MQTTOPEN 最多尝试次数（含第一次）。 */
#define APP_CFG_MODEM_MQTT_OPEN_ATTEMPTS       (3U)
/*! MQTT 拆除（`AT+MQTTDISC` / `AT+MQTTDEL`）遇到 8002 时的重试间隔。
*  NOTE (2026-09-27)：手册（MN316 AT 命令用户手册 §11.1，第 132 页）把 8002 定义为
*  「操作不允许，通常为状态机不符合」——即刚断开、模组还没释放完，**等一会儿再发就好**。
*  原先把它写成 tolerate（一步放行），会跳过本步所有重试，等于在模组没准备好的时候
*  直接进入 `AT+MQTTCFG`。 */
#define APP_CFG_MODEM_MQTT_TEARDOWN_RETRY_MS   (1000U)
/*! 两条 AT 命令之间的最小非阻塞间隔。
*  NOTE (2026-09-27)：手册（MN316 通信流程示例，第 16 页）原文要求
*  「每次只能发送单个 AT 命令，只有前一个 AT 命令执行完毕，才能执行下一个 AT 命令」。
*  本工程原来是「收到上一条 OK 的同一个 Process 周期内就写下一条」，中间零间隔，
*  正是手册里 `+CME ERROR:8007`（AT 通道忙，上一条 AT 请求尚未处理完）的成因。
*  留一小段静默时间给模组收尾，代价是整条计划慢几秒。 */
#define APP_CFG_MODEM_AT_GAP_MS                (200U)

/*! 公网可达性探测（裸 TCP，只读诊断） ------------------------------------------
*  NOTE (为什么要有它，2026-09-27)：三张卡里移动那张稳定连上、两张电信卡一律倒在
*  `AT+MQTTOPEN`。而 `+MQTTOPEN:FAIL` 的手册定义只有一句「连接服务器失败」，区分不了
*  「这张卡根本出不了公网」与「只是连不上我们这台服务器」。本组步骤用模组自带的裸
*  TCP 去连一个**与业务无关**的公网地址，把这两件事分开。
*  NOTE (判据)：`AT+NSOCO` 回 OK **只表示「开始连接」**（手册原文「命令解析正确，开始
*  连接远程服务器」）；真正的证据是 `AT+NSOSD`（**发送 TCP 数据**，AT 命令手册 §5.6）
*  之后收到 `+NSOSTR`——手册批注「发送成功并**收到对方 ACK**」。所以成功标记取
*  `+NSOSTR`，不是 OK。
*  NOTE (命令名别写错，2026-09-27 已踩过)：是 `AT+NSOSD`，**不是** `AT+NSOD`。
*  《MN316_通信流程示例》第 22 页的示例把它写成了 `AT+NSOD`，那是笔误；模组没有这条
*  命令，发错会立刻回 `+CME ERROR:4`（不支持的操作），看着非常像「连不上」。 */
/*! 1 = 在连接计划里插入这四步探测；**0 = 整组从计划表里去掉**（不是「跳过」：
*  计划表用 #if 包住这四行，所以 `plan_count` 与 `step k/N` 的编号完全回到
*  未加探测时的那张表（当前 **23 步**），行为与加它之前一模一样）。详见 modem_mn316.c 里的 NOTE。
*  2026-09-27 结案后已置 0（结论：电信卡侧限制，见改动日志 #45）；以后再遇到
*  「某张卡连不上」时把它改回 1 即可复用这套对照方法。 */
#define APP_CFG_MODEM_NET_PROBE_ENABLE         (0U)
/*! 探测目标。默认阿里 DNS 的 53 端口——国内一定可达，且与业务服务器无关。
*  想验证「能不能连上我们自己的服务器」时，把这里换成该服务器的 IP 与端口。 */
#define APP_CFG_MODEM_NET_PROBE_HOST           "223.5.5.5"
#define APP_CFG_MODEM_NET_PROBE_PORT           (53U)
/*! 本端监听端口（AT+NSOCR 的 <listen port>）。换一个不与别处冲突的值即可。 */
#define APP_CFG_MODEM_NET_PROBE_LOCAL_PORT     (6003U)
/*! 连上之后留给模组建链的时间（毫秒）：`AT+NSOCO` 只表示「开始连」，必须等一会儿
*  再发 `AT+NSOSD`，否则数据发不出去（手册要求先保证连接成功）。 */
#define APP_CFG_MODEM_NET_PROBE_SETTLE_MS      (3000U)
/*! 探测步骤的应答超时（毫秒）。 */
#define APP_CFG_MODEM_NET_PROBE_TIMEOUT_MS     (10000U)
/*! `AT+NSOSD` 的重试次数与间隔：TCP 在 NB-IoT 上建链慢，头几次发不出去是正常的，
*  所以给几次重发机会；`+NSOSTR` 一旦回来就是成功。 */
#define APP_CFG_MODEM_NET_PROBE_SEND_ATTEMPTS  (3U)
#define APP_CFG_MODEM_NET_PROBE_SEND_RETRY_MS  (3000U)
/*! `AT+NSOCL` 的重试次数与间隔。实测连不上目标时 socket 会卡在 connecting，
*  关闭也会被回 `+CME ERROR:8002`；多试几次给模组内部超时留出释放时间，
*  否则每轮连接计划漏一个 socket，两轮之后 NSOCR 就会失败（模组只有 2 个名额）。 */
#define APP_CFG_MODEM_NET_PROBE_CLOSE_ATTEMPTS (4U)
#define APP_CFG_MODEM_NET_PROBE_CLOSE_RETRY_MS (2000U)
/*! 连接失败后 AO 的重连节奏。 */
#define APP_CFG_MODEM_CONNECT_ATTEMPTS         (3U)
#define APP_CFG_MODEM_RECONNECT_PERIOD_S       (10U)
/*! 在线期周期。 */
/*! 业务参数 `TRANSINTVL`：周期上报间隔（分钟），1~20，出厂默认 20。 ---------------
*  NOTE (单一真相，2026-09-28)：设备**实际上报周期**由参数存储里的 `transintvl` 决定
*   —— `AO_Modem` 每个 Process 周期从参数影子读一次（`App_Store_Value()`），因此
*   「改完立即生效」且掉电保持（参数表见 `App/service/svc_param.c`）。
*   旧的云属性通道 `sample_interval`（见下面那组 1~1440 的宏）自本轮起**不再驱动周期**，
*   只保留「读回来核对」的作用：写它不会改变上报节奏。 */
#define APP_CFG_MODEM_TRANS_INTERVAL_MIN          (1U)
#define APP_CFG_MODEM_TRANS_INTERVAL_MAX          (20U)
#define APP_CFG_MODEM_TRANS_INTERVAL_DEFAULT      (20U)
/*! 参数表的编译期默认值（`SVC_Param` 的 desc 表只接字符串）。
*  NOTE (必须与 _DEFAULT 同步)：两个宏要一起改，否则「介质空白时读到的默认值」与
*   「范围回落值」会给出两个不同的数。 */
#define APP_CFG_MODEM_TRANS_INTERVAL_DEFAULT_TEXT "20"
/*! 默认上报周期（秒）：由 `TRANSINTVL` 的出厂默认值换算，只给 `ModemConfig.report_period_s`
*  用。组件里那份 `report_period_ms` 是信息性的，真正的节奏由参数影子决定。 */
#define APP_CFG_MODEM_REPORT_PERIOD_S          (APP_CFG_MODEM_TRANS_INTERVAL_DEFAULT * 60U)
#define APP_CFG_MODEM_CELL_REFRESH_PERIOD_S    (60U)
/*! 旧云属性通道 `sample_interval` 的范围；该通道已降级为只读镜像（去留待平台确认）。 */
#define APP_CFG_MODEM_SAMPLE_INTERVAL_MIN      (1U)
#define APP_CFG_MODEM_SAMPLE_INTERVAL_MAX      (1440U)

/*! 在线期上行的容错与模组自愈（2026-09-20 新增） ---------------------------------
*  NOTE (为什么加)：实机实测「云链路正常跑 20 分钟后，一条周期上报被模组回错」，
*  原实现把这一条上行失败直接升级成整条链路作废 → 进重连阶梯 → 而模组当时已掉网
*  且不再自己搜网 → 3 次尝试用尽后**永久沉默**，只能人工上电。下面这组把这条路径
*  补成「抖动先重试 → 射频软复位 → 断电重启 → 长期重试」。 */
/*! 运行期上行（周期上报 / 命令应答）失败后的**重试次数**：重试用尽才判链路失效并重连
*  （即一条上行最多尝试 1 + 本值 次）。实测「一次抖动就整条链路作废」是 20 分钟掉线的
*  直接原因，所以这里给足缓冲。 */
#define APP_CFG_MODEM_UPLINK_RETRY             (3U)
/*! 上行重试的退避间隔（毫秒）；太短会把重试次数在几十毫秒内用光。 */
#define APP_CFG_MODEM_UPLINK_RETRY_MS          (5000U)
/*! 连接次数用尽后是否给模组断电重启（1 = 允许；需要硬件有电源控制脚，如 MN316_EN）。 */
#define APP_CFG_MODEM_POWER_CYCLE_ENABLE       (1U)
/*! 断电时长（毫秒）：够模组彻底掉电复位即可，过长只会拖慢恢复。 */
#define APP_CFG_MODEM_POWER_OFF_MS             (1000U)

/*! 云端连接与设备身份；通用组件内不得写死这些值。
*   NOTE (凭据例外)：明文凭据的登记与三条纪律见 AGENTS.md 第 4.2 节；
*   日志与错误路径一律不得打印用户名与密码。
*   NOTE (2026-09-27 已回退)：`#29` 曾为了把「模组侧 DNS」与「网络可达性」分开，
*   把这里临时改成纯 IP `"43.154.189.189"`。实验已经得出结论（换成 IP 后现象完全一样
*   → DNS 不是原因），因此这里**已改回域名**。不要再用 IP：平台换地址或走 CDN 后就失联，
*   IP 直连也绕过了域名带来的调度与容灾。 */
#define APP_CFG_MODEM_HOST                     "www.xiaoqingchong.com"
#define APP_CFG_MODEM_PORT                     (48040U)
#define APP_CFG_MODEM_PRODUCT_ID               "sensor-product"
/*! 设备标识（MQTT `clientId` 与主题里的 `deviceId`）：**不再用编译期宏**，见下。
*
*  NOTE (为什么删掉 `APP_CFG_MODEM_DEVICE_ID`，2026-09-28)：它原先是写死的
*    `"869413084698157"`——那是**某一台模组的 IMEI**。一旦 `deviceId` 参与主题拼装，
*    写死就等于**所有设备共用同一个身份**：同一批设备同时上线会因 `clientId` 相同而
*    互相顶掉（MQTT 后到者踢掉先到者），平台侧表现为"设备随机掉线、数据归属混乱"。
*    现在 `deviceId` 取自参数 `sn`（产线用串口刷写、掉电保存），因此**一份固件即可跑所有设备**。
*    取值点：`App/app/modem.c` 的 `Modem_ctor()` → `App_Store_Value(SVC_PARAM_BLE_SN)`；
*    合法性判据：`SVC_Param_IsValidDeviceId()`（命令层写入与 4G 启动共用同一个函数）。
*  NOTE (未配置就不上线)：`sn` 为空、含非法字符、或仍是出厂默认值（= 产线没刷）时，
*    `AO_Modem` **拒绝启动 4G** 并在串口打出 `device SN unusable …` 告警。
*    这是故意的：非法值拼出的 `sensor-product//command` 在 MQTT 里**是合法主题**，
*    模组订阅还会回 `+MQTTSUBACK`，只是永远收不到平台消息（与 `#51` 同类静默故障）。
*  NOTE (平台侧必须同步)：平台侧那台设备的 `deviceId` 必须与设备 `SN` **逐字一致**
*    （含大小写），否则本设备订的主题平台不认——同样是"订阅成功但没反应"。 */
#define APP_CFG_MODEM_USERNAME                 "jetlinks0621"
#define APP_CFG_MODEM_PASSWORD                 "jetlinks0621"
#define APP_CFG_MODEM_KEEP_ALIVE_S             (60U)

#endif /* APP_CONFIG_H */
