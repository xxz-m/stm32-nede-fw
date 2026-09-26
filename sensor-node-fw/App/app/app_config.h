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

/*! 产品身份与小程序配置；通用组件内不得写死这些值。 */
#define APP_CFG_BLE_SN                        "VWA2608A0013"
#define APP_CFG_BLE_NAME                      "VWA2608A0013"
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
#define APP_CFG_MODEM_MQTT_STATE_TIMEOUT_MS    (30000U)
#define APP_CFG_MODEM_TOPIC_TIMEOUT_MS         (20000U)
/*! 连接失败后 AO 的重连节奏。 */
#define APP_CFG_MODEM_CONNECT_ATTEMPTS         (3U)
#define APP_CFG_MODEM_RECONNECT_PERIOD_S       (10U)
/*! 在线期周期。 */
#define APP_CFG_MODEM_REPORT_PERIOD_S          (1200U)
#define APP_CFG_MODEM_CELL_REFRESH_PERIOD_S    (60U)
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
#define APP_CFG_MODEM_DEVICE_ID                "869413084698153"
#define APP_CFG_MODEM_USERNAME                 "jetlinks0621"
#define APP_CFG_MODEM_PASSWORD                 "jetlinks0621"
#define APP_CFG_MODEM_KEEP_ALIVE_S             (60U)

#endif /* APP_CONFIG_H */
