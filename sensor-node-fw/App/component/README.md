---
title: 可移植通信组件规范
status: 进行中
owner: 待填
updated: 2026-09-29
---

# 可移植通信组件规范

`App/component/` 保存可跨 MCU、内核和具体通信芯片复用的通信能力。组件核心只能依赖标准 C 和自身 `port` 接口，禁止包含 HAL、CMSIS、QP、RTOS 头文件，禁止使用全局可变状态和动态内存。平台状态全部由调用者传入的实例与 `context` 持有。

## BLE 分层

```text
ble_component.*        对 App 的唯一公共接口、连接与业务状态
ble_protocol.c         20 B 帧、CRC8、分片、认证、参数与上报
ble_port.h             write/read/get_tick_ms/trace 平台端口
backend/ble_vg6328.c   VG6328 AT、URC 与 UUID 配置差异
```

## 交付与移植

对外提供整个 `App/component/ble/` 目录，不提供本工程的 `App/app/ble.c`、`App/bsp/`、QP/C、STM32 HAL 或任何串口生成代码。接收方应把该目录加入自己的工程，并按下面顺序接入。

可直接照抄的示例位于 `ble/examples/`：`ble_component_config.example.h` 是独立配置头；`ble_baremetal_example.c.example` 展示 UART 和四个 Port 回调；`ble_business_example.c.example` 专门展示上层怎样配置产品身份与参数、更新传感器数据、响应上位业务的 set、立即及周期上报。

### 1. 配置组件

本工程在 `ble_component_config.h` 中包含 `app_config.h`，并将下列工程宏映射到组件宏。接收方没有 `app_config.h` 时，应删除这一行 include，并直接在 `ble_component_config.h` 给右侧组件宏赋值；不需要修改 `ble_component.c`、`ble_protocol.c` 或 backend 源码。

| 本工程配置 | 组件内部宏 | 含义 |
|---|---|---|
| `APP_CFG_BLE_BACKEND` | `BLE_COMPONENT_BACKEND` | 编译期芯片后端选择 |
| `APP_CFG_BLE_TRACE_ENABLE` | `BLE_COMPONENT_TRACE_ENABLE` | 组件诊断信息开关 |
| `APP_CFG_BLE_TRACE_RX` | `BLE_COMPONENT_TRACE_RX_ENABLE` | 把模组发来的每一行原样打进日志（排障开关，默认开；见第 6 节） |
| `APP_CFG_BLE_JSON_MAX` | `BLE_COMPONENT_JSON_MAX` | 单条完整 JSON 最大字节数 |
| `APP_CFG_BLE_MAX_PARAMS` | `BLE_COMPONENT_MAX_PARAMS` | 参数表最大项目数 |
| `APP_CFG_BLE_TX_DEPTH` | `BLE_COMPONENT_TX_DEPTH` | 待发送 JSON 队列深度 |
| `APP_CFG_BLE_FRAME_MAX` | `BLE_COMPONENT_FRAME_MAX` | BLE 单帧最大长度，当前协议固定为 20 B |
| `APP_CFG_BLE_FRAGMENT_DATA_MAX` | `BLE_COMPONENT_FRAGMENT_DATA_MAX` | 每个分片的数据长度，当前协议固定为 14 B |
| `APP_CFG_BLE_RX_BUDGET` | `BLE_COMPONENT_RX_BUDGET` | 每次 `Process` 最多读取的串口字节数 |
| `APP_CFG_BLE_COMMAND_TIMEOUT_MS` | `BLE_COMPONENT_TIMEOUT_MS` | 单条 AT 指令响应超时 |
| `APP_CFG_BLE_RESTART_WAIT_MS` | `BLE_COMPONENT_RESTART_WAIT_MS` | 模组复位后的等待时间 |
| `APP_CFG_BLE_RESTART_PROBE_ATTEMPTS` | `BLE_COMPONENT_RESTART_PROBE_MAX` | 模组复位后的 AT 复测次数 |

SN、PIN、蓝牙名称、Service UUID、设备名称、型号和批次不是库内宏。它们由产品工程构造 `BleConfig` 后传入，避免同一份库固化某个客户或设备身份。参数名称、单位、范围和读写属性由产品工程构造 `BleParam[]` 后传入。

> [!note] 本工程（`sensor-node-fw`）的取法：**广播名 = SN**（2026-09-28）
> 组件本身把 `serial_number` 与 `local_name` 当作两个独立字段（别的客户可能真的要分开），
> 但本工程的产品决定「蓝牙广播名就是设备 SN」，因此 `ble.c` 让两者**同指** `SVC_PARAM_BLE_SN`：
> 单一真相、物理上不可能不一致，产线只刷一条 `AT+SN=`。独立参数槽位 `name` 已退役
> （见 `App/service/svc_param.h` 的 NOTE 与 [[docs_sensor/08-改动日志]] `#76`）。
> 下面的示例保留两个不同的值，是为了展示**组件能力**，不代表本工程的取法。

### 2. 业务层怎样配置和上报

下面是组件上方的产品业务最小示例。型号、批次、SN、PIN、广播名和 UUID 都由业务层填写，不需要修改 BLE 驱动或协议源码：

```c
static BleComponent ble;
static BleParam params[5];

static BleConfig const product = {
    .serial_number = "VWA2608A0013",
    .pin           = "062500",
    .local_name    = "VWA2608A0013",
    .service_uuid  = "CDAB0000-1234-5678-1234-56789ABCDEF0",
    .device_name   = "VWA-0013",
    .model_code    = "VWA-LB01",
    .batch_code    = "2026Q3-A01"
};

static char const *const report_names[] = {
    "frequency", "temperature", "signal_quality",
    "voltage", "sample_interval"
};
```

业务参数在初始化时登记。每一项包含协议名称、显示名称、单位、当前值、是否允许小程序修改、最小值、最大值、修改回调和回调上下文：

```c
params[0] = (BleParam){"frequency", "Frequency", "Hz",
    {BLE_VALUE_INT, {.integer = 0}}, false, 0, 100000, 0, 0};

params[1] = (BleParam){"temperature", "Temperature", "C",
    {BLE_VALUE_DECIMAL, {.decimal = {0, 2U}}},
    false, -4000, 12500, 0, 0};

params[2] = (BleParam){"signal_quality", "Signal quality", "%",
    {BLE_VALUE_INT, {.integer = 0}}, false, 0, 100, 0, 0};

params[3] = (BleParam){"voltage", "Voltage", "mV",
    {BLE_VALUE_INT, {.integer = 3300}}, false, 0, 5000, 0, 0};

params[4] = (BleParam){"sample_interval", "Sample interval", "s",
    {BLE_VALUE_INT, {.integer = 60}}, true, 1, 86400,
    sampleIntervalChanged, &product_context};

BLE_Component_Init(&ble, &port, &product, params, 5U);
BLE_Component_Start(&ble);
```

传感器产生新数据时，业务层只更新参数当前值。温度使用带两位小数的定点数，`2261` 会在 JSON 中表示为 `22.61`：

```c
params[0].value.value.integer = frequency_hz;
params[1].value.value.decimal.mantissa = temperature_cC;
params[2].value.value.integer = signal_percent;
params[3].value.value.integer = voltage_mv;
```

需要主动发送一包 `data` 时，传入需要上报的参数名；组件会查表取当前值、生成 JSON、分片和加 CRC：

```c
if (BLE_Component_IsReady(&ble)) {
    bool queued = BLE_Component_Report(
        &ble,
        report_names,
        sizeof(report_names) / sizeof(report_names[0]));
}
```

`queued=true` 表示完整 JSON 已进入组件发送队列，不代表手机已经确认收到。业务层可在认证成功后立即调用一次，以后按 `sample_interval` 周期调用；告警或按键触发也可以调用同一个接口。小程序执行 `set` 时，组件先检查类型、范围和 writable，再调用业务提供的 `BleParamChanged`：

```c
static int sampleIntervalChanged(char const *name,
                                 BleValue const *value,
                                 void *context) {
    ProductContext *product = (ProductContext *)context;
    (void)name;
    product->report_period_ms =
        (uint32_t)value->value.integer * 1000U;
    return 0;  /* 0 接受；非 0 拒绝这次 set */
}
```

可复制的完整版本见 `ble/examples/ble_business_example.c.example`。

### 3. 实现平台端口

接收方只需实现一个 `BlePort`。四个回调都在调用者上下文执行，组件自身不创建线程，也不进入中断：

```c
static ble_port_status_t uartWrite(void *ctx, uint8_t const *data, size_t len);
static size_t uartRead(void *ctx, uint8_t *data, size_t capacity);
static uint32_t tickMs(void *ctx);
static void traceText(void *ctx, char const *text); /* 可选，允许置 NULL */

static BlePort port = {
    &my_uart,
    uartWrite,
    uartRead,
    tickMs,
    traceText
};
```

- `write()` 必须非阻塞，并且只能“整段接收”或返回 `BLE_PORT_BUSY/ERROR`，禁止只接收前半段后返回成功。
- `read()` 从接收环形缓冲取出当前已有字节，最多写入 `capacity`，没有数据立即返回 0。
- `get_tick_ms()` 返回单调递增的 32 位毫秒值，允许自然回绕，禁止在组件里用阻塞延时。
- `trace()` 只接收已经格式化好的 ASCII 文本。可接日志队列、串口 printf 包装或置空；不得在串口 ISR 中调用。
- UART RX ISR 只把字节放进接收环形缓冲；UART TX ISR 只推进发送队列。解析、AT 状态机和业务回调都由 `BLE_Component_Process()` 完成。

### 4. 构造并推进

所有对象由接收方静态分配。初始化一次，等待模组上电稳定后启动，并在主循环、定时任务或 RTOS 线程中频繁调用 `Process`：

```c
static BleComponent ble;
static BleParam params[5];
static BleConfig config = {
    "VWA2608A0013", "062500", "VWA2608A0013",
    "CDAB0000-1234-5678-1234-56789ABCDEF0",
    "VWA-0013", "VWA-LB01", "2026Q3-A01"
};

BLE_Component_Init(&ble, &port, &config, params, 5U);
BLE_Component_Start(&ble);

for (;;) {
    BLE_Component_Process(&ble);
    /* 裸机建议每 5~20 ms 调用一次；这里不得使用组件内部阻塞等待。 */
}
```

连接和认证状态分别用 `BLE_Component_IsConnected()` 与 `BLE_Component_IsReady()` 查询。只有 `IsReady()` 为 true 后才能调用 `BLE_Component_Report()`。断链后组件会自动清除认证、半包、发送队列和会话状态，调用方不应保留“已认证”缓存。

### 5. 加入构建系统

必须编译 `ble_component.c`、`ble_protocol.c` 和且仅一个 `backend/ble_<chip>.c`，头文件搜索路径加入 `ble/` 与 `ble/backend/`。组件只使用 C99 标准头 `<stdbool.h>`、`<stddef.h>`、`<stdint.h>`、`<stdio.h>` 和 `<string.h>`；不依赖 STM32、HAL、QP 或 RTOS。低 RAM 平台应优先缩小 JSON、参数数量和 TX 深度，并重新核算 `sizeof(BleComponent)`。

### 6. VG6328 首次联调

UART 配置为 115200 8N1。先确认 `AT` 探测成功，再确认名称、UUID、广播字段与 MAC 查询完成；手机连接后依次验证 notify 订阅、SN+PIN 认证、参数发现、get/set、首包 data、周期 data、ping、断链清理与重连。若 trace 出现 CRC error，先核对手机端是否严格发送“2 B 大端长度 + payload + CRC8 0x07”，以及 20 B 帧和 14 B 分片规则。

`BlePort.write()` 必须非阻塞且整包接收：空间不足返回 `BLE_PORT_BUSY`，不得只接收部分数据。`read()` 立即返回已有数据；`get_tick_ms()` 必须允许无符号回绕；`trace()` 可为空实现。组件由 `BLE_Component_Process()` 增量推进，任何调用都不得等待模组响应。

调试由工程级 `APP_CFG_BLE_TRACE_ENABLE` 控制，经 `ble_component_config.h` 映射为组件内部的 `BLE_COMPONENT_TRACE_ENABLE` 和 `BLE_TRACE(component, text)`。输出只经过注入的 `BlePort.trace()`；组件不得直接调用 `printf`。当前 STM32 适配把 trace 接到现有异步 `LOG_D`，因此还会受 `APP_CFG_LOG_ENABLE` 与 `APP_CFG_LOG_LEVEL` 控制，任一开关关闭后对应调用在编译期消失。

另有 `APP_CFG_BLE_TRACE_RX`（→ `BLE_COMPONENT_TRACE_RX_ENABLE`，2026-09-28 加）：**把模组发来的每一行原样打进日志**，用于区分「手机压根没连上」与「连上了但那条通知我们不认识」——`BLE_Protocol_Line()` 对认不出的行是静默丢弃的，没有它现场只能猜（4G 侧的 `APP_CFG_MODEM_TRACE_URC` 是同款开关，当初靠它抓出 `#51`）。含 `{` 的行是小程序 JSON 载荷（auth 请求带 PIN），**只打占位符**。

后端用 `BLE_COMPONENT_BACKEND` 编译期选择。更换蓝牙芯片时只新增 `backend/ble_<chip>.c`，不得复制或改写小程序协议、参数模型和上报逻辑。产品 SN、PIN、名称、UUID、型号与批次通过 `BleConfig` 传入；通用组件中禁止固化凭据。

当前工程所有可调项统一放在 `App/app/app_config.h` 的 `APP_CFG_BLE_*` 区域，包括功能/后端/trace、UART 与环形缓冲、JSON/参数/发送队列/帧/分片/RX 预算，以及启动、AT 超时、探测重试、模组重启和上报周期。`ble_component_config.h` 只负责把这些工程宏映射到组件命名空间；迁移到 AT32、GD32 或 51 时只需调整这一个配置头并实现 `BlePort`。

VG6328 后端对 BTM、ADE、名称、ULEN、UUIDX 和广播字段先查询再比较，匹配时跳过写入，不匹配才修改。配置检查结束后发送旧工程已验证的 `AT+RST=1`，等待 `APP_CFG_BLE_RESTART_WAIT_MS`，并最多用 `AT` 复测 `APP_CFG_BLE_RESTART_PROBE_ATTEMPTS` 次；复测通过后查询 MAC。当前每次启动都重启一次模组，避免旧固件已经写入配置、却未重启生效的过渡状态。整个过程由时间与 RX 事件推进，不阻塞 QV。

当前协议为：2 B 大端长度 + 最多 17 B 载荷 + CRC8（多项式 `0x07`）；总帧最大 20 B。长 JSON 使用 `序号 + 2 B 总长 + 14 B 数据` 分片。支持 `auth`、`params`、`get`、`set`、`ping`、`nak`、`reboot`、`data`，认证前拒绝业务命令；断链清除认证、重组和发送会话。

### 7. BLE 日志的两级粒度（配置阶段 / 连接之后）

BLE 的日志按**阶段**分成两档，理由是这两个阶段要看的东西完全不同（2026-09-19 按使用反馈调整）：

**一、配置阶段（手机还没连进来）——只判成败。** 这个阶段现场只需要回答「模组配好了没有」这一个问题，所以 `ble_component.c` 的 `traceConfig()` 在成功时只打「第几步 + 正在写哪一项」：

```text
ble-core: config 1/16 cmd="AT"
ble-core: config 2/16 cmd="AT+BTM?"
ble-core: config 4/16 cmd="AT+ADE?"
ble-core: config 14/16 cmd="AT+RST=1"
```

只有**写失败 / 应答 `ERROR` / 超时**才把命令、应答与原因一起打出来：

```text
ble-core: config 4/16 fail cmd="AT+ADE=1" rx="ERROR" reason=3
ble-core: config 6/16 fail cmd="AT+NAM=VWA2608A0013" rx="" reason=2
```

- `config k/16`：`k` 是配置表下标（从 1 起），分母是后端的总步数（VG6328 为 16）。**注意步号会跳**——查询步（`?`）匹配上时对应的设置步（`=`）会被跳过，所以看到 `2/16` 之后直接是 `4/16` 是正常的，不代表漏发。
- `cmd`：这一步正在写的 AT 命令（成功行上也有，用来知道走到哪一项）。
- `reason` 是 `BleFailure`：0 无 / 1 串口写不进 / 2 超时 / 3 应答 `ERROR`。
- 命令文本按当前步号**重新生成**（`BLE_Backend_Command()` 是纯函数），所以失败点不需要额外保存它。

**二、连接之后——逐条可见。** 手机连上来之后的流量才需要细看，`ble_protocol.c` 会打出收到的请求与发出的应答摘要：

```text
ble-core: link connected
ble-core: rx cmd="auth"
ble-core: tx cmd="auth" len=44
ble-core: rx cmd="get"
ble-core: tx cmd="get" len=71
ble-core: rx cmd="set"
ble-core: tx cmd="data" len=96
ble-core: link disconnected
```

**只打 `cmd` 与长度，绝不打 JSON 原文**——`auth` 请求里带设备 PIN，运行期日志不得出现凭据（见 `AGENTS.md` 5.2 与凭据纪律）。要知道具体是哪一项，看设备应答里的 `param` 字段。

顺带补了三条原本**完全静默**的路径，手机侧表现为「发了没反应、设备日志一片安静」，是最难定位的一类：

| 日志 | 含义 |
|---|---|
| `rx invalid request` | 收到的 JSON 解析不出 `cmd` 字段（对端格式不对） |
| `rx json too long` | 单包 JSON 超过 `BLE_COMPONENT_JSON_MAX`，整包被丢弃 |
| `rx frag dropped` | 分片乱序、总长不符或超长，整包重组被放弃 |
| `tx dropped` | 发送队列满或单包超长，应答没发出去（手机侧表现为「没收到应答」） |

日志开关同第 6 节：`APP_CFG_BLE_TRACE_ENABLE` 编译期整体消失；接到 `LOG_D`，`APP_CFG_LOG_LEVEL` 调到 `INFO` 也能静音。

## 当前 STM32 接线

`AO_Ble` 是 QP 适配者：以 `APP_CFG_BLE_PROCESS_PERIOD_MS` 时间事件推进组件，通过动态事件接收 `AO_Sensor` 的不可变采样快照。BSP 在 `APP_CFG_BLE_UART_ID` 指定的那一路串口（**当前 USART2 = PA2/PA3**，2026-09-20 与 4G 对调）上提供单生产者/单消费者 RX 环形缓冲和中断 TX 队列，并在初始化时拉高 `VG6_EN`(PB1) 给模组上电。VG6328 不存在或配置超时时，BLE 进入失败状态并只记录一次日志，不影响其他 AO。

## 4G 分层

```text
component/modem/
├── modem_component.*       对 App 的唯一公共接口、连接状态机与业务协议
├── modem_component_config.h 工程宏 → 组件宏的唯一映射点
├── modem_port.h            write/read/get_tick_ms/trace 平台端口
├── modem_at.*              芯片无关的增量 AT 引擎（喂行 + 判期限）
└── backend/
    ├── modem_backend.h     后端内部接口（对 App 不可见）
    ├── modem_ml307c.c      度云 Cat.1 DTU：通道 + 主题登记 + 路由发送（AT+RESET 激活通道）
    ├── modem_nt26.c        度云 Cat.1 bis DTU：通道 + 主题登记 + 路由发送
    └── modem_mn316.c       中移芯昇 NB-IoT：主题字符串 + 十六进制负载
```

每个固件只编入一个后端（`#if` 排除其余两份，不产生代码也不占 Flash）。业务协议（逻辑主题、上报报文、下行 `ping/get/set` 分发，以及平台 AT 命令下行的拆包与应答拼装）只出现在 `modem_component.c`；具体 AT 命令、URC 与错误映射只能出现在 `backend/`。后端不持有任何全局可变状态，主题序号、重试游标与补救标志都放在 `ModemComponent` 里。

核心是**非阻塞**的：`Modem_Component_Process()` 只做「读走串口已有字节 → 组行喂给 AT 引擎 → 比较期限」，三步分别在三次调用里发生，任何一次调用都不等待模组。连接过程被表达成一张线性**步骤表** `ModemStep`，每步给出命令、期望应答标记、应答超时、重发间隔与总预算；必要时还可在本步成功后增加非阻塞稳定等待：

| 执行方式 | 语义 |
|---|---|
| `MODEM_STEP_ONCE` | 发一次；失败即整条计划失败 |
| `MODEM_STEP_OPTIONAL` | 发一次；失败或超时也继续（诊断 / 兼容类命令） |
| `MODEM_STEP_REPEAT` | 失败后按 `retry_ms` 重发，直到尝试次数与 `budget_ms` 都用尽 |
| `MODEM_STEP_TOPIC` | 按 `topic_cursor` 逐条下发主题，没有更多主题时自动推进整步 |

MN316 在清理 MQTT 客户端之前先发送 `AT+MQTTSTAT?`，并按手册（MQTT 用户手册 §2.3）的取值分三档：状态 5 = 已连接，说明 MCU 虽然重启但模组云会话仍在，核心跳过 `MQTTDISC` / `MQTTDEL` / `MQTTCFG` / `MQTTOPEN` 直接进入主题处理；状态 3/4/6/7 = 发 Connect 等 ack / 正在重连 / 建 TCP 中 / TCP 已建立，属于**连接正在路上**，只按 `APP_CFG_MODEM_MQTT_STATE_POLL_MS` 重问而不打断，等满 `APP_CFG_MODEM_MQTT_STATE_TIMEOUT_MS` 仍未到 5 才按未连接继续；状态 0/1/2 = 明确未连接，直接走重建。重建路径中，`AT+MQTTCFG` 成功后不会立即发送 `AT+MQTTOPEN`：先用 `APP_CFG_MODEM_MQTT_OPEN_SETTLE_MS` 做非阻塞稳定等待；若收到 `+MQTTOPEN:FAIL`，再按 `APP_CFG_MODEM_MQTT_OPEN_RETRY_MS` 退避，最多尝试 `APP_CFG_MODEM_MQTT_OPEN_ATTEMPTS` 次。重试用尽后仍交给 AO 的整套重连与断电恢复流程，不把失败误判成 `READY`。

`AT+MQTTDEL` 的 `+CME ERROR:8002` 在手册（AT 命令用户手册 §11.1，第 132 页）里的定义是「操作不允许，通常为状态机不符合」——是可等待的瞬时状态，因此计划表把它声明成 `MODEM_STEP_OPTIONAL_REPEAT`：按 `APP_CFG_MODEM_MQTT_TEARDOWN_RETRY_MS` 重试 3 次，用尽后继续下一步而不是判整条连接失败。相邻两条 AT 之间另有 `APP_CFG_MODEM_AT_GAP_MS` 的最小间隔，对应手册「只有前一个 AT 命令执行完毕，才能执行下一个 AT 命令」的要求。

### 公网可达性探测（`netprobe`，2026-09-27）

`+MQTTOPEN:FAIL` 的手册定义只有一句「连接服务器失败」，它**区分不了**两件结论完全相反的事：这张卡根本出不了公网（定向卡 / 策略拦截），还是只是连不上我们这台服务器。为此在 `AT+CGCONTRDP`（PDP）之后插入了四步**裸 TCP 探测**，用模组自带的 socket 能力去连一个**与业务无关**的公网地址：

```text
AT+NSOCR="STREAM",6,6003,1     → +NSOCR:<id> / OK   netprobe socket <id> created
AT+NSOCO=<id>,223.5.5.5,53     → OK                 netprobe connecting 223.5.5.5:53 (start)
AT+NSOSD=<id>,4,41424344       → +NSOSD:<id>,4      netprobe … TCP send accepted (socket connected)
AT+NSOCL=<id>                  → OK                 netprobe socket <id> closed
```

> [!warning] 两条命令写法都是踩过坑才定下来的
> **① 命令名是 `AT+NSOSD`，不是 `AT+NSOD`。** 《MN316_通信流程示例》第 22 页的示例写成了
> `AT+NSOD`，**那是笔误**；AT 命令用户手册第 5 章（第 78~85 页）的命令表里只有
> `5.2 AT+NSOST`（发 UDP）和 `5.6 AT+NSOSD`（发 TCP）。发错名字会**立刻**回
> `+CME ERROR:4`（不支持的操作）——「立刻」很关键，它不是超时。
> **② 只用三个参数，不要填 `<flag>`/`<sequence>`。** 语法是
> `AT+NSOSD=<socket>,<length>,<data>[,<flag>[,<sequence>]]`，第 4 个位置是 `<flag>`，
> 合法值只有 `0x100/0x200/0x400`；填 `4`（想当 `<sequence>` 用、好换回 `+NSOSTR`）会被回
> `+CME ERROR:8001`（输入的 AT 命令参数不合法）。第 84 页示例 `AT+NSOSD=0,5,ABCDEF1234,1`
> 与它自己的语法自相矛盾，别照抄示例。三参数的**成功响应是手册明写的
> `+NSOSD:<socket>,<length>`**，用它当成功标记就够了。
> 数据格式是**十六进制**——由 `AT+NSOCFG` 的 `<input_mode>` 确认（手册第 86 页：「发送数据
> 时使用的数据格式，**默认为十六进制**」）。

**判据**：`AT+NSOCO` 回的 `OK` **不是**「连上了」——手册 §5.5（第 83 页）对它的批注是「命令解析正确，**开始**连接远程服务器」。真正能说明问题的是 `AT+NSOSD` 的回执：拿到 `+NSOSD:<id>,<len>` 就说明模组已经把数据交给一条**已建立的 TCP 连接**；若 socket 还停在 connecting，会回 `+CME ERROR:8002`（操作不允许，状态机不符合）。

读法：

| 现象 | 结论 |
|---|---|
| 出现 `netprobe … TCP send accepted (socket connected)` | 这张卡**能上公网**（TCP 已建立）→ 问题在业务服务器/平台那一侧 |
| `AT+NSOSD` 回 `+CME ERROR:8002`（操作不允许，状态机不符合） | socket 卡在 connecting → SYN 没得到回应 → **卡出不了公网** |
| 连 `netprobe socket N created` 都没有 | `AT+NSOCR` 就没通过，先查 socket 是否被上次占用（手册第 24 页：最多 2 个） |

四个步骤全部是 `OPTIONAL`（`SEND` 为 `OPTIONAL_REPEAT` ×3），**探测失败绝不影响后面的 MQTT 流程**，只打日志（`LOG_D`，受 `APP_CFG_LOG_LEVEL` 与 `APP_CFG_MODEM_TRACE_ENABLE` 两级过滤）。

> [!warning] 当前状态：**已关闭**（`APP_CFG_MODEM_NET_PROBE_ENABLE = 0`）
> 这套探测在 2026-09-27 查出结论后已经关掉，**默认不参与编译进计划表**——那四步是用 `#if` 包住的，
> 所以关闭时 `plan_count` 与 `step k/N` 的编号**完全回到 `#if` 之外的那张表**（当前 **23 步**，
> `AT+MQTTOPEN` 是第 **21** 步），行为与加它之前一模一样。以后遇到「某张卡连不上」时把该宏改回 `1` 即可复用同一套对照方法；
> 打开后编号会整体 +4（`AT+MQTTOPEN` 变第 25 步）、成功路径约多 6~7 s，属诊断态。
> 探测目标在 `APP_CFG_MODEM_NET_PROBE_HOST` / `_PORT`（想改成自家服务器时只改这两行）。

连接计划完成后进入 `READY`：先消化已收全的下行通知（分发 `ping` → `pong`、`get` → 读属性应答、`set` → 写配置应答），再按需发布应答与周期数据。**上行发布失败不再立刻判定链路不可用**（2026-09-20 改）：先按 `MODEM_COMPONENT_UPLINK_RETRY` / `MODEM_COMPONENT_UPLINK_RETRY_MS` 退避重试，重试用尽才转入 `FAILED` 交回上层重连。原因：实测 NB-IoT 上一次上行抖动就会让「一条上报失败 = 整条链路作废」，设备 20 分钟后进重连阶梯并因模组掉网未自愈而永久沉默。

对 App 的公共入口固定为 `Modem_Component_Init/Start/Process/RequestReport/RequestCellInfo/RequestLocation/IsReady`，外加平台 AT 命令下行用的 `Modem_Component_TakeCommand/CompleteCommand`。`Start()` 可反复调用，用作失败重连；`RequestReport()` 与 `RequestCellInfo()` 只置请求位，不在调用点等待。

### 平台 AT 命令下行（`command` / `command/reply`）

2026-09-27 起按平台规则对接了一条新的下行通道：订阅 `{productId}/{deviceId}/command`，
回复发布到 `{productId}/{deviceId}/command/reply`（**不带**前导 `/`，见下）。

> [!warning] 前导 `/` **按主题逐条给**（2026-09-28 订正）
> `buildTopics()` 的拼法是 `"%s%s/%s%s"`：**最前面那个 `/` 取自 `kTopicPrefix[]`**——一张
> **一条主题一行**的表，表里每一行对应 `app_config.h` 里**一个宏**（`APP_CFG_MODEM_TOPIC_PREFIX_*`，共 10 个，
> 只能填 `""` 或 `"/"`，有编译期护栏）；device 与后缀之间的 `/` 由 `kTopicSuffix[]` 自带。
> **平台要求某一条换形态时，只改 `app_config.h` 那一行，不必碰组件代码。**
>
> **当前取值**：**只有数据上报那一条**（`properties/report` → `/sensor-product/<did>/properties/report`）带 `/`；
> 其余 9 条（5 条订阅 + 4 条应答，含 `command/reply`）**都不带**。
> 依据是平台侧的**两个互不相干的主题空间**：数据上报页原文要求订阅 `/{productId}/{deviceId}/properties/report`（带 `/`）；
> 命令交互（「AT 控制主题模板」「回复主题模板」）写的是 `{productId}/{deviceId}/…`（不带 `/`）。
>
> **为什么不再"按方向"一刀切**：平台侧确实有两栏会各自变动，但**"方向"不是那件事的维度**——
> `#58`→`#62` 就是因为把它当成"发布方向统一带 `/`"，给 `command/reply` 多加了一个 `/`：
> 设备发 5 段主题、平台按 4 段过滤器认 → 平台上停在 `WAITING_RESPONSE`、20 s 判"指令超时"（**已实测**）。
> 逐条成表后，平台单独要求某一条换形态时，只改 `kTopicPrefix[]` 里那一行。
>
> 历史（别重复踩）：`#48` 把格式串由 `"/%s/%s/%s"` 改成 `"%s/%s%s"`，**顺带把 10 条主题的前导 `/` 全去掉了**
> （`#48` 写「旧主题保持现网路由不变」，**是错的**，见 `#50`）；`#50`/`#51` 按当时实机现象记成「与当前平台一致」；
> `#58` 一度改成全体带 `/`，`#60` 改为按方向用宏，`#64` 才收敛为**按主题**。
>
> **改前缀必须与平台页面同步**：订阅串一旦与平台发布串对不上，下行命令会**静默**失效（`+MQTTSUBACK` 照样回来，表现为「设备在线、订阅齐全、平台发布无回应」）。
> 上板要看的：`AT+MQTTSUBACK` 里的主题串，以及从平台发一条 `AT` 能否回 `{"id":…,"ok":true,"data":"OK\r\n"}`。

组件在这条通道上**只做两件事**，不执行命令：

| 步骤 | 谁做 | 说明 |
|---|---|---|
| 拆包 | 组件 `dispatchDownlink()` | 用 `jsonStringUnescape()` 取出 `id` 与 `command`（转义感知：写参数命令自带引号，普通提取会在 `\"` 处截断），置 `downlink_pending` |
| 取走 | App 层轮询 `Modem_Component_TakeCommand()` | 组件不认识参数存储，也不许依赖 QP，所以执行必须上交 |
| 执行 | App 层（`AO_Modem`） | 命令文本喂 `SVC_At_Feed()` → `App_Command_Dispatch()`，与串口**完全同一条路** |
| 回包 | 组件 `Modem_Component_CompleteCommand()` | 由它拼单层 JSON（含 `\r`/`\n`/`"`/`\` 转义）并排进上行队列 |

应答格式按规则 3.3 / 3.4：成功是 `{"id":…,"ok":true,"data":"…OK\r\n"}`，失败是 `{"id":…,"ok":false,"error":"ERROR","data":"…ERROR\r\n"}`。`data` 是**完整原始 AT 回复**并按 JSON 转义；本设备的失败形态是 `+CME ERROR:<n>`，交给 App 层时会在它后面补一行 `ERROR` 收尾（规则 4.2 允许「可选错误行 + ERROR」）。

> [!note] 主题拼装的两处 `/` 别弄混（2026-09-28 订正）
> 拼法是 `"%s%s/%s%s"`：**最前面那个 `/` 来自 `kTopicPrefix[]`（按主题逐条给，命令通道不带）**；
> product 与 device 之间的 `/` 是格式串里的分隔符；**device 与后缀之间的 `/` 由 `kTopicSuffix[]` 各自携带**——
> **只改一处、另一处漏改，就会拼出「中间少一个斜杠」或「多一个前导斜杠」的错主题**，
> 而模组对订阅照样回 `+MQTTSUBACK`，症状是"设备在线、订阅齐全、平台没反应"。

| 本工程配置 | 组件内部宏 | 含义 |
|---|---|---|
| `APP_CFG_MODEM_BACKEND` | `MODEM_COMPONENT_BACKEND` | 编译期芯片后端选择 |
| `APP_CFG_MODEM_TRACE_ENABLE` | `MODEM_COMPONENT_TRACE_ENABLE` | 组件诊断信息开关 |
| `APP_CFG_MODEM_COMMAND_SIZE` | `MODEM_COMPONENT_COMMAND_MAX` | 单条 AT 命令最大字节数 |
| `APP_CFG_MODEM_RESPONSE_SIZE` | `MODEM_COMPONENT_RESPONSE_MAX` | 单条命令的应答留档缓冲 |
| `APP_CFG_MODEM_PAYLOAD_SIZE` | `MODEM_COMPONENT_PAYLOAD_MAX` | 上行/下行 MQTT 负载缓冲 |
| `APP_CFG_MODEM_URC_SIZE` | `MODEM_COMPONENT_URC_MAX` | 主动通知（URC）收包缓冲 |
| `APP_CFG_MODEM_LINE_SIZE` | `MODEM_COMPONENT_LINE_MAX` | 单行文本装配缓冲 |
| `APP_CFG_MODEM_TOPIC_SIZE` | `MODEM_COMPONENT_TOPIC_MAX` | 单个主题名字节数 |
| `APP_CFG_MODEM_RX_BUDGET` | `MODEM_COMPONENT_RX_BUDGET` | 每次 `Process` 最多读取的串口字节数 |
| `APP_CFG_MODEM_DOWNLINK_ID_SIZE` | `MODEM_COMPONENT_DOWNLINK_ID_MAX` | 平台下行 `id` 的保存长度（原样回显） |
| `APP_CFG_MODEM_DOWNLINK_COMMAND_SIZE` | `MODEM_COMPONENT_DOWNLINK_COMMAND_MAX` | 平台下行 `command` 的保存长度（**小于平台的 256**：本设备 AT 行上限只有 64，存更长没有意义） |
| `APP_CFG_MODEM_COMMAND_TIMEOUT_MS` | `MODEM_COMPONENT_TIMEOUT_MS` | 单条命令默认应答超时 |
| `APP_CFG_MODEM_SIM_WAIT_MS` 等 | `MODEM_COMPONENT_*` | 入网、附着、通道与重连各阶段预算 |
| `APP_CFG_MODEM_SAMPLE_INTERVAL_MIN/MAX` | `MODEM_COMPONENT_SAMPLE_MIN/MAX` | 可写参数 `sample_interval` 的范围 |

服务器地址、端口、产品标识、设备标识、用户名与密码不是库内宏。它们由产品工程构造 `ModemConfig` 后传入，避免同一份库固化某个客户或设备身份。凭据只出现在 AT 命令里，绝不进入日志、断言或错误路径。

### ML307C-DTU 后端差异

`modem_ml307c.c` 与 `modem_nt26.c` 同属度云 DTU 方言（通道 + 主题登记 + 路由发送），大部分命令与 `+RECV,<通道>,"<主题>",<数据>` 的 URC 格式实测一致，但下面五处必须区分——照抄 NT26 会直接跑不通（均为 2026-09-17 在 ML307C-GC-RTU-2.1.40 上实测确认）：

| 项 | NT26 | ML307C-DTU |
|---|---|---|
| 激活通道参数 | `AT+RTUCTL=<ch>,1` | 无此指令，只能 `AT+RESET` 后按落盘配置自建链（实测约 11.6 s 出 `+MQTT-CONNECT`） |
| 上行发布 | `AT+RTUWRITE="<ch>[<n>]",<数据>` | `AT+SENDR="<ch>[<n>]",<数据>`；`AT+SEND` 同义但**无任何应答**，不能用于需要确认的路径 |
| 发布主题登记 | 比 ML307C 多一个 retain 参数 | `AT+MQTTPUB=<ch>,<n>,"<主题>",<qos>`，共 4 个参数 |
| 定位方式 | 计划表里有 `AT+LBSCFG="lbs_mode",2`（切定位模式），但**没有读回坐标的步骤**，钩子留空 | 无 `AT+LBSCFG`；改用本模组自带的 `AT+LBS`，它自己算基站定位并直接回经纬度，已实现（见下文「基站定位」） |
| `AT+CGI` 字段顺序 | 第 1 个当小区标识、第 2 个当 TAC | **与 NT26 相同**：第 1 个当小区标识、第 2 个当 TAC。手册写作 `"<lac>","<cell_id>"`，但它自己的示例 `"85be00a","1f52"` 与实机三次采样都是「7 位十六进制在前、4 位在后」，而 TAC 是 16 bit、小区标识是 28 bit——手册把两个名字写反了。判据见 `modem_ml307c.c` 的 `ml307cCellApply()` |
| MQTT 认证参数 | 无此命令 | `AT+MQTTAUTH=<id>,"<client_id>","<用户名>","<密码>",""`：**必须给 4 个字符串参数**（auth1..auth4），只发 3 个会回 `+CME ERROR: 4` |

还有四点是本后端特有的取舍，改代码前先读懂：

- **配置顺序**：`AT+MQTT`（服务器地址与端口）必须排在 `AT+MQTTPLATFORM` 之前——实机跑通的序列与厂默认值的存储顺序都是这样（计划表原先写反，2026-09-19 修正）。
- **每次连接都会复位模组**：`MODEM_OP_CHANNEL_OPEN` 发 `AT+RESET`，因此比 NT26 多约 12 s；`MODEM_OP_CHANNEL_CLOSE` 步被删掉（没有指令可关通道，而实测在通道已使能状态下写 MQTT 参数是可接受的）。要省掉这 12 s 得给 `ModemComponent` 加「配置未变则跳过复位」的标志位，属于核心改动，本轮不做。
- **查询类步骤一律用精确期望标记**（`+IMSI:` / `+CSQ:` / `+CEREG:` / `+ISLINK:` / `+DTUSTATE:` / `+CGI:` / `+MQTTSUB:` / `+MQTTPUB:` / `+SENDR:0`）。因为 `ModemAt_LineHasToken()` 是子串匹配，而本模组重启后会主动上报 `+NET: "OK"`，用 `"OK"` 作标记会被它抢答；`MODEM_OP_CHANNEL_STATE` 步一旦被抢答，核心会直接进入 `READY`，而通道其实还没连上。
- **`AT+SENDR` 用 `+SENDR:0` 而不是 `"OK"` 判成功**：模组先回 `+SENDR:0`（推入发送区）或 `+SENDR:-1`（推送失败），之后才是 `OK`；用 `"OK"` 会把 `-1` 也算成功，形成「看起来发出去了其实没有」的假成功。

**已知隐患（本轮未处理）**：`ModemAt_LineHasError()` 用子串匹配 `ERROR`，而本模组会用 `+NET: "ERROR"` 主动上报掉网；该行恰在某条命令在途时到达，那条命令会被误判为 `ERROR`。影响有限（真掉网本来就该失败重连），根治要把判定改成行首匹配，属于跨后端的公共改动，另行评估。

### MN316 后端差异（2026-09-19 实机核对）

MN316 是**标准 3GPP 系**模组（`+CPIN?` / `+CGATT?` / `+CGDCONT?` / `+CGPADDR` 等标准指令都可用），与 ML307C-DTU 那种「厂商私有 DTU 方言」完全不同。以下几条是**对着真机逐条核过**的结果，改这个后端前先看：

| 项 | 实测结果 | 对代码的影响 |
|---|---|---|
| 串口速率 | **9600**（115200 完全静默） | `APP_CFG_MODEM_UART_BAUD` 的 MN316 档必须保持 9600 |
| 位置信息 | `AT+CEREG=2` 之后 `AT+CEREG?` → `+CEREG:2,1,6E55,02516E85,9`，**裸十六进制、不带引号** | `mn316CellApply()` 用 `ModemAt_NextField()`（不能用 `NextQuoted()`，会读不到） |
| 小区标识宽度 | CI 是 **8 位零填充**十六进制（如 `02516E85`），与 ML307C 的 7 位 ECI 形态不同 | 平台侧解析注意宽度，不要按 7 位写死 |
| MCC / MNC | `AT+COPS?` → `+COPS:0,2,"46000",9`，**可取** | 若要做平台侧小区反查，MN316 能直接提供 mcc/mnc |
| MQTT 打开 | `AT+MQTTOPEN=1,1,0,0,0,"",""` 先回 `OK`，约 3 s 后**异步**回 `+MQTTOPEN:OK` | 期望标记就是 `+MQTTOPEN:OK`，靠它而不是那声 `OK` 判成功 |
| MQTT 状态 | 已连接时 `AT+MQTTSTAT?` → `+MQTTSTAT:5` | `apply` 判 `value == 5`，与真机一致。**但 5 只是"会话还活着"**，能不能复用还要看下一条 |
| MQTT 配置读回 | `AT+MQTTCFG?` → `+MQTTCFG:183.230.40.39,6002,4069959,60,75829,***,*,1,0`（host, port, **clientId**, keepalive, user, 掩码密码…；**密码由模组自己打星号**） | `MODEM_OP_MQTT_CFG_QUERY` 用它读回 host/port/clientId 与当前身份比对：**一致才允许复用会话**（`remote_cfg_matches`）。手册 §2.1 第 9 页。不同型号的字段个数可能有差异，解析只取前 3 个 |
| MQTT 订阅 | `AT+MQTTSUB="<主题>",<qos>,0` → `OK`，随后异步 `+MQTTSUBACK:<id>,<qos>,<主题>` | **期望标记必须用 `+MQTTSUBACK`**（代码就是这么写的）。不要图省事改成 `OK`：`OK` 只说明命令被受理，**不代表订阅真的成功了**——`#51` 就是因为只看 `OK` 而错过「订错主题」，白跑一整轮 |
| 下行通知 | **2026-09-27 首次实测确认**：`+MQTTPUBLISH:<a>,<b>,<c>,<d>,<主题>,<负载字节数>,<负载>`，例如 `+MQTTPUBLISH:0,1,0,1,sensor-product/869413084698157/command,67,{"id":"…","command":"AT"}`（命令通道的主题**不带**前导 `/`，与 `kTopicPrefix[]` 当前对 `MODEM_TOPIC_COMMAND` 的取值一致） | `mn316UrcStarts/LocatePayload/UrcComplete` 三件套都是按这个形态写的，**名字与字段位置已对上真机**；负载按声明字节数收全（不按换行），因为负载里可能含换行 |
| MQTT 操作超时 | 订阅迟迟没回 `+MQTTSUBACK` 时，模组会报 `+MQTTTO:<n>`（超时窗口由计划里的 `AT+MQTTTO=20` 设定） | 见到 `+MQTTTO:` 说明会话在超时，不是命令写错；`rx="OK|+MQTTTO:3"` + `reason=2` 就是这个场景 |
| 上行发布 | `AT+MQTTPUB="<主题>",<qos>,0,0,<原长>,"<HEX>"` → **`OK`**（QoS 0/1/2 都是这一条），**负载是 HEX 编码** | `mn316PublishBuild()` 用 `ModemAt_HexEncode()`，改动时别把编码去掉。**期望标记恒为 `OK`**——手册 §2.5（第 14 页）写明成功响应就是 `OK`，且**没有 ACK 型 URC**（对照 §2.4 `+MQTTSUBACK`、§2.6 `+MQTTUNSUBACK`；发布的 QoS2 流程只有 `+MQTTPUBREC/REL/COMP`）。曾经按 QoS 选标记、给 QoS 1 等 `+MQTTPUBACK`，导致 4 条 `*/reply` 每条都超时（`#59`，2026-09-28 修） |
| 上行发布失败 | `AT+MQTTPUB` 失败回 `+CME ERROR:<err>` 或 `ERROR` | 引擎按 `ERROR`/`+CME` 判失败；**只有 `OK` 才算成功**，别再加别的期望标记 |
| 基站定位 | `AT+LBSCFG` → `+CME ERROR: 4`，**本模组无此指令** | MN316 计划表里本来就没有 `MODEM_OP_LBS_MODE`（NT26 才有），且定位钩子为空 |
| `AT+WORKLOCK` | 只支持写入（`AT+WORKLOCK=1` → OK），查询形式回 `+CME ERROR:8001` | 不要用 `AT+WORKLOCK?` 判状态 |
| 设备标识 | `AT+CGSN=1` → `+CGSN: "869413084698157"`（**当前固件读完就丢**） | 2026-09-28 起 MQTT 的 `clientId`/`deviceId` 取自**参数 `sn`**（产线串口刷写），不再是模组 IMEI；这一步只是 `MODEM_OP_IDENTITY` 的**诊断**用途（`mn316Apply()` 里没有 IDENTITY 分支）。将来若要把身份改回"读模组 IMEI"，就在那里补解析 |

### 基站定位（LBS）

定位与小区信息一样，属于「从网络侧派生的补充数据」，都是**可选**的：拿不到就整条键不出现在报文里，绝不影响在线状态。上报字段为 `location_lon` / `location_lat`，单位 **1e-6 度**的定点整数（`ModemMeasurements.lon_e6` / `lat_e6`，见 [[07-接口约定#数据类型与单位]]）。

- **触发**：`modem.c` 在小区补查的同一个 tick 里调 `Modem_Component_RequestCellInfo()` 与 `Modem_Component_RequestLocation()`——两者同频（`APP_CFG_MODEM_CELL_REFRESH_PERIOD_S`），平台也是一起消费的，没有必要再开一个计时器。请求只置标志位，AT 往返由组件在下一个 `Process` 周期异步发起。
- **ML307C 实现**：`AT+LBS` → `+LBS: "<经度>","<纬度>"`，单位是**十进制度文本**（手册示例 `+LBS: "104.071319","30.690876"`），由 `ml307cLocationApply()` 用 `ModemAt_ParseE6()` 折成 1e-6 度整数，绕开 Cortex-M3 上没有硬件浮点的浮点库。经纬度各解析一次、**两个都成功才提交**，避免出现「经度已更新、纬度还是上一次的」这种不配对的组合；范围校验为经度 ±180、纬度 ±90，越界当次作废；`(0,0)` 视为无效。
- **失败是常态，不是异常**：模组侧定位请求最长要 5 s，室内或基站稀疏时直接回 `+CME ERROR: 130`（手册自己的示例就是失败）。因此核心按**尽力而为**处理：只清请求标志，**不动连接状态、也不清掉上一次取到的有效坐标**。为此这一步的超时用的是 `MODEM_COMPONENT_CONFIG_TIMEOUT_MS`（10 s），必须压得住模组侧的 5 s——不能用查询类那档很短的超时。
- **后端不支持时不发命令**：`ModemBackend.location_build` / `location_apply` 两个钩子同时为空即表示本后端不做定位，核心直接把请求丢掉（NT26 / MN316 当前如此），而且**不打 trace**——请求是按周期来的，每次打一条会把日志刷满。NT26 计划表里的 `AT+LBSCFG="lbs_mode",2` 只是把模组切成多基站融合定位模式，本后端没有读回坐标的步骤，等于「配了没消费」，要补齐得先确认该固件用哪条指令取经纬度。
- **怎么判读日志**：`4g-core: location unavailable` = 这次没定位成功（正常，可忽略）；`4g-core: location parse failed` = 模组答了但格式不是预期的两个引号字段，那才是要查的。

### 连接过程的诊断输出

连接计划有十几个步骤，成功路径上不打日志，上板时很容易只看到「一直没动静」。因此 `modem_component.c` 在每一步的**结论处**打一条 trace（走 `ModemPort.trace`，受 `APP_CFG_MODEM_TRACE_ENABLE` 与 `APP_CFG_LOG_LEVEL` 控制，编译期可整体消失）。

**成功只打进度，命令与应答只在异常路径出现**（2026-09-19 按使用反馈调整）：成功时每步都带上 `cmd`/`rx` 会把串口刷满——连接计划十几步（ML307C 17 步、**MN316 23 步**——2026-09-27 在 `IP_ADDRESS` 之后插了一步只读的 PDP/APN 诊断、2026-09-28 又在 `MQTT_STATE` 之前插了一步 `AT+MQTTCFG?` 读回比对），主题步还要按主题逐条展开，一次连接就是二十多行；而排查时真正要看的只是「卡在第几步」。所以（下面的例子取自 ML307C 的 17 步，`N` 以实际日志为准）：

```text
4g-core: connecting, 17 steps                                  ← 连接开始时给出总步数
4g-core: step 1/17 ok
4g-core: step 6/17 ok
4g-core: step 14/17 ok topic 2/4                               ← 主题步补 topic k/n，否则同一行会重复四次
4g-core: step 17/17 wait cmd="AT+DTUSTATE=1" rx="+DTUSTATE: 1,0" reason=3
4g-core: step 17/17 fail cmd="AT+DTUSTATE=1" rx="+DTUSTATE: 1,0" reason=3
4g-core: step 17/17 write-fail cmd="AT+DTUSTATE=1" rx="" reason=1
```

- `step N/M`：计划表下标（从 1 起）/ 计划步数。**成功行与异常行用同一套编号**，所以「卡在第几步」直接对上；
- `topic k/n`：主题步（`MODEM_STEP_TOPIC`）专用的进度。主题步不推进 `plan_step`、只推进 `topic_cursor`，不加这个就分不出走到第几条主题；
- `cmd` / `rx`：这次尝试发出去的 AT 命令与收到的应答，**只在异常行上打**。
  - `cmd` 字段**故意只有 36 字符**：`AT+MQTTCFG=` 里带着用户名与密码，截在 host 之后正好把凭据挡住。要看清尾部得先给组件加「可安全打印的命令」缓冲，**不要直接把这个上限调大**（等于把凭据写进日志）。
  - `rx` 打的是应答的**首行|末行**（两行相同时只打一次）。为什么不是只打首行：AT 应答是多行的——模组先回命令确认（`OK`），随后才用 **URC** 上报真正的结果。实例：`AT+MQTTOPEN` 的命令响应只有 `OK`，而连接成功/失败分别由 URC `+MQTTOPEN:OK` / `+MQTTOPEN:FAIL` 上报；**只打首行会把「模组明确报了连接失败」显示成 `rx="OK" reason=2`（超时）**，2026-09-27 上板就被这么误导过一次。现在会打成 `rx="OK|+MQTTOPEN:FAIL"`。
  - `rx=""` 表示模组一个字都没回，优先查接线、供电与波特率。
- `tag` 与 `reason`：
  - `ok` = 这一步通过了（只打进度，不带 `cmd`/`rx`）；
  - `wait` = **问到了但还没就绪，稍后会再问**——不是故障。轮询步（`MODEM_STEP_REPEAT`，如 `+CEREG` / `+ISLINK` / `+DTUSTATE`）在预算内会反复出现这一行；
  - `resend` = 重发同一命令（上一次没答上来，或答了但值还不满足）；
  - `recover` = 跑完预算仍未就绪，已执行后端的补救动作（ML307C 为再来一次 `AT+RESET`）；
  - `fail` = 这一步真的失败了：要么是不可重试的步骤，要么轮询步的预算已用尽、计划即将整体失败；
  - `write-fail` = 串口一直写不出去。
- `reason` 是 `ModemFailure`：0 无 / 1 串口写不进 / 2 超时 / 3 应答不符 / 4 缓冲溢出 / 5 计划组装失败。`wait` 行上的 `reason` 是**本轮没通过的原因**（3 = 答了但值不对，2 = 压根没答），可用来区分「模组不在线」与「模组在线但还没连上」。

**怎么判读**：`ok` 行在**递增**（`1/17 → 2/17 → …`）就是在正常推进；只有 `wait` / `resend` 说明在正常等待，**不要当成故障**——本后端的 `AT+RESET` 之后模组要约 12 s 才建链，这段时间 `step 17/17 wait … rx="+DTUSTATE: 1,0"` 是必然现象（判据是等到 `+DTUSTATE: 1,1`）。**同一行反复出现同一编号**、或者出现 `fail`，才是卡点。

**PDP/APN 那一行（2026-09-27 新增）**：MN316 的 `MODEM_OP_PDP` 步（只读、`OPTIONAL`）在成功时由后端自己打一条 trace，把**已激活上下文**的 APN 与默认网关打出来。它有**四种**形态，必须分清楚：

```text
4g-core: pdp apn="ctnb.MNC011.MCC460.GPRS" gw="10.x.x.1"   ← 解析出的 APN + 网关
4g-core: pdp raw="+CGCONTRDP: 1,5,|ctnb…|,|10.x.x.x…|,…"   ← 永远追加一条原文（关键判读依据）
4g-core: pdp apn EMPTY (active context has blank APN)       ← 活动上下文里 APN 也是空的
4g-core: pdp raw="…"
（整步没有任何 pdp 行）                                      ← 模组对 AT+CGCONTRDP=0 回了 ERROR（不支持）
                                                            该步被 OPTIONAL 容忍跳过，apply 不会被调用
```

> [!important] 为什么解析成功也要打原文
> 上板拿到过 `gw=none`，而 `none` 有两种来源：**模组真没给网关**（= 没有到公网的路由，
> 那「TCP 连不出去」就有直接解释）与**模组把网关给成不带引号的字段**（3GPP 规定带引号，
> 但实现有差异；`mn316ReadQuotedField()` 只认引号字段）。两者在日志上同形，结论却相反，
> 所以必须把 `+CGCONTRDP:` 那一行原样打出来。**判读时以 `raw=` 为准。**

> [!warning] ⚠ 本结论已于 2026-09-27 被实测**推翻**，保留原文仅作排查过程记录
> 下一条 `#40` 之后的实测（见 [[08-改动日志]] 的 `#41`）显示：**移动卡在同样 `gw=none`
> 的情况下连上了云平台**（两次冷启动均 `cloud link ready`），电信卡也在另一次冷启动里
> 3.36 s 就连上。因此「没有默认网关 → 包出不去 → TCP 静默超时」这条推断**不成立**：
> `gw=none` 是这些卡/这套网络对 `+CGCONTRDP` 的**常规呈现**，不是故障。
> 真正稳定复现的差异在**电信卡这一侧**。2026-09-27 用公网可达性探测拿到了干净对照
> （同一块板、同一份固件、同一个目标 `223.5.5.5:53`，只换卡）：
>
> | 卡（APN） | 结果 |
> |---|---|
> | 移动 `cmiot.mnc004` | `netprobe … TCP send accepted (socket connected)` → 后续 `cloud link ready` |
> | 电信 `ctnb.MNC011` | `AT+NSOSD` 回 `+CME ERROR:8002`（操作不允许，状态机不符合）→ socket 卡在 connecting |
>
> 两张卡都是 `gw=none`，电信卡注网/附着/拿 IP 也全部正常，却**连不到任意公网地址**。
> 因此结论是：**该电信卡的数据通路被限制（定向卡 / 签约限制），属运营商侧，不是固件问题**。
> 下面的逐字段解读仍然有效（网关字段确实是空的、原文格式确实是那样），失效的只是由它
> 推出的结论。

> [!success] 本次联调结论（2026-09-27，**已被上方实测推翻**）：PDP 上下文**没有默认网关**——问题定位到网络/卡侧
> 实测日志：
>
> ```text
> 4g-core: pdp apn="ctnb.MNC011.MCC460.GPRS" gw=none
> 4g-core: pdp raw="0,5,"ctnb.MNC011.MCC460.GPRS","26.146.250.50.255.0.0.0","","218.…"
> ```
>
> 逐字段读（`+CGCONTRDP: <cid>,<bearer_id>,"<apn>","<local_addr and subnet_mask>","<gateway>","<dns1>",…`）：
> `cid=0`、`bearer=5`、APN = `ctnb.MNC011.MCC460.GPRS`（**中国电信 NB-IoT 的标准公网 APN 形态**：
> MCC 460 = 中国、MNC 011 = 中国电信）、本地地址 = `26.146.250.50` / 掩码 `255.0.0.0`、
> **网关 = `""`（空）**、网关之后是 DNS（`218.…`，有下发）。
>
> 由此可确定：
> 1. **网关确实是空的**，不是解析器的问题（原文里是显式的 `""`）；
> 2. ~~设备只有 `26.0.0.0/8` 这一个直连网段，而 MQTT 服务器 `43.154.189.189`（腾讯云段）
>    **不在其中** → 没有默认路由 → 包出不去 → TCP 连接**静默超时**~~ ← **此条已被推翻**，
>    见上方警告：`gw=none` 时移动卡照样连上，说明该字段不能用来判断"有没有公网路由"；
> 3. ~~因此这**不是固件问题、不是 DNS、更不是服务器问题**——网络给了 IP 和 DNS，
>    却没有给到公网的路由。~~ ← **此条已被推翻**。固件侧确实存在问题（见 `#38`/`#39`/`#40`：
>    只认 MQTTSTAT=5 会拆掉正在建立的连接、8002 被一步放行、AT 之间零间隔）；
>    同时电信卡与移动卡的表现确实不同。
>
> **当前（2026-09-27 晚）的定位**：问题收敛到**电信卡侧**——移动卡多次测试正常，电信卡
> 多次重连后仍停在 `+MQTTREC:n`（模组自身重连）。下一步是向售卡方确认该卡是**公网卡
> 还是定向卡**，而不是继续在固件里找原因。

**用的是 `AT+CGCONTRDP=0` 而不是 `AT+CGDCONT?`**（2026-09-27 第二步切换）。原因：`AT+CGDCONT?` 先跑过一轮，答复是**APN 字段为空**，它只说明「没有**静态配置**的 APN」，**回答不了「网络实际在用哪个 APN」**。而 `AT+CGCONTRDP=0` 回的是**已激活上下文**的参数（`<cid>,<bearer_id>,"<apn>","<local_addr>","<gateway>","<dns1>",…`），一次给出实际 APN + 本地 IP + 网关 + DNS，正好覆盖要判读的三件事：APN 是公网名还是定向专网名；有没有网关（有 IP 没网关 = 没有公网路由）；DNS 是否下发。

为什么要它：上板实测模组能注册、能附着、能拿到 IP，却连不上 MQTT 服务器（`AT+MQTTOPEN` 回 `OK` 后收到 **`+MQTTOPEN:FAIL`**，手册 §2.2 即「连接服务器失败」），而同一端口从 PC 上是通的、域名与纯 IP 都一样失败（DNS 已排除）。问题因此被夹在「模组 → 服务器」这一跳，APN 是头号嫌疑——而**本计划表从不配置 APN**。APN 名不是凭据，可以打进日志。

> [!warning] 实现这个解析时的两个坑
> ① **不要用 `ModemAt_NextQuoted()`**：它对**空字段**返回 false（`modem_at.c:169` 的 `length == 0`
> 分支），会把「APN 为空」误判成「没有这个字段」。这里改用 `mn316ReadQuotedField()`，
> 空字段返回 true 且输出空串。
> ② **两个命令的字段顺序不同**：`+CGDCONT` 的第 1 个引号字段是 **PDP 类型**（`"IP"`），
> 第 2 个才是 APN；而 `+CGCONTRDP` 的**第 1 个就是 APN**。照抄会取到 `"IP"`。
> ③ 第一版只分「parsed / unparsed」两态，上板打出来就是 `pdp apn unparsed`——
> 诊断步骤自己没给出诊断信息。三态 + 失败带原文之后才可用。

嫌太吵时有两级开关：把 `APP_CFG_LOG_LEVEL` 从 `APP_CFG_LOG_LVL_DEBUG` 调到 `APP_CFG_LOG_LVL_INFO`（这些行走 `LOG_D`），或把 `APP_CFG_MODEM_TRACE_ENABLE` 设为 `0U`（编译期整块消失，连 Flash 一起省回）。

计划失败时 `AO_Modem` 会打一行汇总：

```text
modem: link failed reason=2 step=18 rx=726 tx_ovf=0 uart_err=0
```

- `step`：**与 trace 的 `step k/N` 同一口径（从 1 起）**。`failure_step` 内部存的是 0 起的计划下标，打印时 +1；2026-09-27 之前这里少加了这个 1，同一次失败会出现「trace 说第 18 步、这行说 step=17」的矛盾；
- `rx`：模组串口累计收到的字节数（只增不减）。
  **`rx=0` 且 `uart_err=0` = 线上一片安静**（没供电、被摁在复位、线没接、或对方根本不应答）；`rx>0` 说明链路是活的，问题在协议/网络层而不是电气层。**注意 `uart_err=0` 单独不能排除波特率问题**（见下）；
- `tx_ovf`：模组串口（当前 USART3）发送环形缓冲溢出次数。持续增长说明**模组不回话**，我们一直在重发、把发送缓冲堆满；
- `uart_err`：模组串口收到的帧错误/噪声/溢出次数（BSP 出错时会丢掉那个字节再重新装载接收，所以出错期间 `rx` 是空的）。
- **NOTE (误判陷阱)**：`uart_err = 0` **不能**排除波特率不匹配。波特率不对时模组收到的是乱码，它根本不会应答，我们这边自然一个字节都收不到，计数也就一直是 0。「`uart_err > 0`」只能**正向**使用——说明确有字节到达但速率或电气不对；`= 0` 时「没有字节」与「波特率不匹配且模组沉默」这两者**无法区分**，只能换个波特率再试。模组串口的波特率按后端分档，见 `app_config.h` 的 `APP_CFG_MODEM_UART_BAUD`；**它由端口的 `BSP_modemUartInit()` 在运行时写进外设**，启动日志打的是实际生效值（`modem: <后端> on USART<n> <波特率> 8N1`）。

### 链路自愈（2026-09-20 新增）

三层手段，从轻到重，**全部非阻塞**（只写串口或引脚电平，靠 `Process()` 的期限推进）：

| 层 | 触发条件 | 动作 | 位置 |
|---|---|---|---|
| 1 上行重试 | `READY` 下一条上行发布失败 | 退避 `UPLINK_RETRY_MS` 重发，最多 `UPLINK_RETRY` 次；用尽才转 `FAILED` | `modem_component.c` 的 `finishRuntime()` |
| 2 射频软复位 | 注册 / 附着步预算耗尽 | 分两阶段：`AT+CFUN=0` → 3 s → `AT+CFUN=1` + `AT+COPS=0`，再给一个完整注册预算 | 后端 `recover()`（现只有 `mn316Recover()`；用 `me->recover_phase` 记阶段） |
| 3 断电重启 | AO 用完 `APP_CFG_MODEM_CONNECT_ATTEMPTS` 次连接 | `MN316_EN`(PB9) 拉低 `APP_CFG_MODEM_POWER_OFF_MS` 再拉高，重装串口接收，从第 1 次重新计 | `App/app/modem.c`（AO）+ `BSP_modemPowerSet()` |

> [!important] 为什么三层都要有
> 2026-09-20 实机实测：链路正常跑 **20 分钟**后一条周期上报被模组回错（`reason=3`），模组随即掉网并停在 `+CEREG:2,0,,,`（**未注册、而且不搜网**）长达十分钟；旧实现只有「失败 → 重连」一条路，3 次尝试用完就**永久沉默**，只能人工上电。层 1 消除「一次抖动就重连」，层 2 让模组不必断电就能回来，层 3 是它彻底卡死时的兜底。
>
> 另外注意：**层 1 的重试次数是"重试"不是"尝试"**——`UPLINK_RETRY = 3` 表示一条上行最多发 4 次（1 次首发 + 3 次重试）。运行期失败现在会打 `runtime fail cmd="…" rx="…" reason=…`，这是定位这类问题的第一手证据（原先只打一行 `cloud link failed`，模组的错误码全丢）。

### Port 契约

与 `BlePort` 完全一致，只有状态码前缀不同（`modem_port_status_t`：`MODEM_PORT_OK/BUSY/ERROR`）：

- `write()` 必须非阻塞，并且只能「整段接受」或返回 `MODEM_PORT_BUSY/ERROR`；只收前半段会让 AT 命令残缺，比返回忙更难排查。
- `read()` 从接收环形缓冲取出当前已有字节，最多 `capacity`，没有数据立即返回 0。
- `get_tick_ms()` 返回单调递增的 32 位毫秒值，允许自然回绕。
- `trace()` 只接收已格式化的 ASCII 文本，可为 NULL；不得在串口 ISR 中调用。

UART RX ISR 只把字节放进接收环形缓冲；TX ISR 只推进发送队列。组行、AT 匹配、URC 解析与业务分发全部由 `Modem_Component_Process()` 完成。

### 新增芯片后端

1. 在 `backend/` 新增 `modem_<chip>.c`，实现 `ModemBackend` 的 `build/apply/recover/urc_*/publish_build/cell_build/cell_apply` 与一张连接计划表；
2. 在 `app_config.h` 增加 `APP_CFG_MODEM_BACKEND_<CHIP>`，并在 `modem_component_config.h` 映射成 `MODEM_BACKEND_<CHIP>`；
3. 在 `backend/modem_backend.h` 的 `#if/#elif/#else` 链与 `Modem_Backend_Get()` 里各加一行；
4. **不要**修改 `modem_component.c` 的业务协议、`modem_at.c` 的 AT 引擎或 Port 契约；
5. 计划表每行的最后一个字段 `tolerate` 用于声明**可容忍的错误串**：有些命令的「失败」恰恰是期望状态——例如 MN316 上没有 MQTT 客户端时 `AT+MQTTDEL` 必然回 `+CME ERROR:8002`，而「没有客户端」正是这一步要的结果。填了它（如 `"+CME ERROR:8002"`）表示「本步收到该错误、且应答里确实出现该串 → 按成功处理」；留 `(char const *)0` 表示任何 `ERROR` / `+CME ERROR` 都算失败。判据写在 `modem_component.c` 的 `connectLoop()` 里，**只在 `MODEM_AT_ERROR` 时生效**，不影响 `apply()` 的正常路径。
   > [!warning] 计划表的 `attempts` 与 `budget_ms` 要配成一对
   > `budget_ms == 0` 的 REPEAT 步完全靠 `attempts` 收敛（每发一次 `attempt++`）。2026-09-20 修掉的一个坑：`connectLoop()` 原先的超期判定在 `budget_ms == 0` 时恒为真，会抢在重发之前反复 `stepFailed`，导致 `attempt` 永不递增、该步**永远停在 `wait` 既不推进也不失败**。同一个坑会影响本表里 `attempts ≥ 2` 的步（MQTTDEL、MQTTCFG）。

### 当前 STM32 接线

`AO_Modem` 是 QP 适配者：以 `APP_CFG_MODEM_PROCESS_PERIOD_MS`（20 ms）时间事件推进组件，通过动态事件接收 `AO_Sensor` 的不可变采样快照，在线后按配置周期置上报与小区刷新请求。BSP 在 `APP_CFG_MODEM_UART_ID` 指定的那一路串口（**当前 USART3 = PB10/PB11**，2026-09-20 与 BLE 对调）上提供单生产者/单消费者 RX 环形缓冲和中断 TX 队列（`BSP_modemUartInit(port, id)/SetSink/RxComplete/TxComplete/Error`），并在初始化时拉高 `MN316_EN`(PB9) 给模组上电（`NB_RESET`/PB8 按用户决定保持悬空）。模组不存在、SIM 未就绪或通道起不来时，组件进入 `FAILED`，`AO_Modem` 只记录一次诊断日志并按配置重连；**连接次数用尽后由 AO 给模组断电重启**（`BSP_modemPowerSet(false/true)`，见上一节的第 3 层），不再永久沉默，不影响其他 AO。`APP_CFG_MODEM_ENABLE=0` 时组件与 BSP 都不参与编译，事件池、sink 注册与 AO 装配一并消失（**2026-09-20 起为 1，模组 MN316，AT 口 9600**）。

## 验证

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\apply_qp_to_keil.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\smoke_build.ps1
```

上板还需逐项验证探测、广播、PIN+SN 认证、参数发现、读写、周期上报、NAK、断链清理和复位；无实测记录时不得标记 L5 通过。
