# AGENTS.md — sensor-node-fw 工作区约束

- **适用范围**：`E:\stm32\sensor-node-fw` 整个工作区（含 `sensor-node-fw/`、`docs_sensor/`）。
- **约束对象**：任何 AI 助手、脚本或自动化流程。
- **效力**：本文件优先于个人习惯。与本文件冲突的改动视为不合规，须先改本文件再改代码。

---

## 1. 读取与编码

| 项 | 规定 |
|---|---|
| 文本编码 | 全部 UTF-8。新建/修改文件一律用文件写入工具，**禁止**用 `Out-File` / `>` / `Set-Content` 写文件（PowerShell 默认 ANSI，会把中文写坏） |
| 含中文的 C/H 与 .ps1 | 必须带 **UTF-8 BOM**。原因：Keil µVision 靠 BOM 判断文件是 UTF-8，没有 BOM 会显示乱码；PowerShell 5 读取无 BOM 的 `.ps1` 会按 ANSI 解码并报语法错误。加 BOM 由 `App/tools/apply_qp_to_keil.ps1` 的第 5 步自动完成 |
| 注释语言 | **统一用中文**（自研代码与文档）。第三方源码里的原有注释保持原样，不做翻译；我们在第三方文件中新增的注释用中文并标注 `NOTE (...)` |
| 读中文文档 | `docs_sensor/*.md`、`*.md` 一律用文件读取工具；不要用裸 `Get-Content`（不带 `-Encoding UTF8`）去读或比较含中文的文件 |
| Keil / CubeMX 工程文件 | `*.uvprojx`、`*.uvoptx`、`*.ioc` 为 CRLF：改动必须保留 CRLF（插入行用 `` `r`n ``），改完必须能通过 XML 解析（`.uvprojx`） |
| 第三方文件 | `Qpc/`、`Drivers/`、`Core/`（生成部分）只做最小 diff，禁止整体重排、重命名或重新格式化 |

---

## 2. 进入项目先读

动手之前按顺序读，读完再改：

1. `AGENTS.md`（本文件，工作区约束）
2. `docs_sensor/00-索引.md`，再按需读 `01-项目介绍` → `07-接口约定`（工程文档，事实来源）
3. `sensor-node-fw/QP_INTEGRATION.md`（QP/C 接入点、AC6 兼容改动、CubeMX 再生成流程）
4. `sensor-node-fw/Qpc/README.md`（QP/C 来源、许可、对本仓库做的端口修改清单）
5. `sensor-node-fw/sensor-node-fw.ioc`（时钟、外设、中断优先级、HAL 时基的**唯一真相**）
6. 要改的层：先看该层目录下的 `README.md`（`App/service/`、`App/driver/` 都写清了职责与边界），再动代码

规则：

- 文档与代码冲突时，**以代码和 `.ioc` 为准**，并顺手修正文档（仅限事实性偏差）。
- **需求与文档冲突时，先提问再动手**（见第 10 节）：不要自行选边改，也不要把冲突悄悄抹平。
- 不要重复推导已有结论（引脚、时钟、中断优先级、内存占用都在 `docs_sensor/03-资源分配.md`）。
- 不确定的硬件事实（板子 LED 位置、晶振频率、传感器型号）**先问，不要假设**。

---

## 3. 仓库结构

```
E:\stm32\sensor-node-fw\
├── AGENTS.md                        本文件：工作区约束入口
├── docs_sensor\                     工程文档集（00-索引.md 为入口，01..13，含 08-改动日志）
├── sensor-node-fw\                  ★ 主工程（STM32CubeMX + QP/C 6.9.1 QV）
│   ├── sensor-node-fw.ioc           CubeMX 配置（时钟/外设/NVIC/时基）
│   ├── Core\                        CubeMX 生成（启动、中断、HAL 时基、外设初始化）
│   ├── Drivers\                     STM32F1 HAL + CMSIS（厂商层）
│   ├── App\                         ★ 自研代码，按分层建目录，CubeMX 不会动
│   │   ├── app\        App 应用层：app.c（装配点 App_start）· command_dispatch.*（统一命令执行）· app_signals.h（信号分段）· app_prio.h（优先级）· node.*（控制台/AT 通道）· sensor.*（采样 AO）· ble.*（AO_Ble）· modem.*（AO_Modem）· store.*（AO_Store，掉电参数存储）
│   │   ├── app_command\ 可移植命令模型：app_command.h（来源、SvcAtCmd、关联 ID、统一结果）
│   │   ├── service\    Service 服务层：svc_filter.*（中值/滑动平均）· svc_frame.*（CRC-16/组帧/解析）· svc_at.*（AT 解析）· svc_log.*（异步日志）· svc_param.*（参数记录编解码）
│   │   ├── driver\     Driver 器件驱动层：drv_types.h（层公共状态码）· drv_sensor.*（统一传感器接口，当前为仿真数据源）· drv_at24c04.*（AT24C04 非阻塞驱动）
│   │   ├── component\  可移植通信组件：业务核心 + 可替换芯片后端，禁止依赖 HAL/QP/RTOS
│   │   │                ble\（VG6328）与 modem\（ML307C/NT26/MN316 + 增量 AT 引擎）
│   │   ├── bsp\        BSP 板级支持层：bsp.*（QP 钩子、时钟节拍）· bsp_uart.*（USART1 + 异步日志）· bsp_ble_uart.*（BLE 端口，实例由 APP_CFG_BLE_UART_ID 决定，当前 USART2）· bsp_modem_uart.*（4G 端口，实例由 APP_CFG_MODEM_UART_ID 决定，当前 USART3）· bsp_i2c.*（I2C1 非阻塞主机，参数存储用；I2C 中断使能与 IRQ 入口在本文件，不在 .ioc）
│   │   └── tools\      工程与验证脚本（apply_qp_to_keil.ps1、smoke_build.ps1、at_smoke.ps1）
│   ├── ThirdParty\     第三方库（<库名>/{inc,src,LICENSE,README.md}，见其 README.md）
│   ├── Qpc\            QP/C 6.9.1 源码 + QV 端口（见其 README.md）
│   ├── MDK-ARM\        Keil 工程（.uvprojx / .uvoptx / project.uvmpw）
│   └── QP_INTEGRATION.md            QP 接入说明 + 再生成检查清单
├── embedded-layering-architecture\  架构说明（HTML，参考资料）
└── .trae-html-share-packages\       工具产物，禁止修改
```

| 目录 | 生成者 | 能否手改 |
|---|---|---|
| `sensor-node-fw/App/` | 人 / AI | ✅ 唯一推荐落点（按 app/service/driver/bsp 分层） |
| `sensor-node-fw/ThirdParty/` | 外部来源 | ✅ 新增库需登记；库源码本身只读 |
| `sensor-node-fw/Qpc/` | QP/C 上游 | ⚠️ 仅编译器兼容必要时，且要登记（见第 4 节） |
| `sensor-node-fw/Core/`、`Drivers/` | STM32CubeMX | ⚠️ 只允许 `/* USER CODE BEGIN..END */` 区域内 |
| `sensor-node-fw/MDK-ARM/*.uvprojx` | CubeMX + µVision | ⚠️ 只允许通过脚本修补（见第 4 节） |
| `sensor-node-fw/MDK-ARM/*.uvoptx`、`.uvguix.*` | µVision | ❌ 由 µVision 维护，不进版本管理 |
| `docs_sensor/` | 人 / AI | ✅ 文档，随代码同步更新 |

---

## 4. 工作区约束

### 4.1 再生成安全（核心约束）

**所有手写代码必须放在 CubeMX 不会删除的位置**，即 `App/`（按分层建子目录）与 `Qpc/`。具体要求：

- 业务逻辑、状态机、BSP 封装、服务与驱动代码、工具脚本 → 只放 `App/` 下对应层的目录。
- 生成文件（`Core/`、`Drivers/`）内**只允许**在 `/* USER CODE BEGIN x */ … /* USER CODE END x */` 之间改动，且越少越好。
- **能落到 `.ioc` 的配置一律落 `.ioc`**（时钟、外设、中断使能与优先级、HAL 时基），不要在生成代码里手改这些值——只有写进 `.ioc` 才不会被下次生成覆盖。
- `MDK-ARM/<工程>.uvprojx` 里的文件组、宏、头文件路径、`uAC6` 会被 CubeMX 重新生成时重置。恢复脚本会**按磁盘实际文件自动发现**分组与头文件路径，因此新增/移动 `.c`、`.h` 不需要改脚本。
- 保留 `MDK-ARM/project.uvmpw`（空文件，用于让 CubeMX 尽量不清空 Keil 工程里的手工分组），**不要删除**。
- 再生成后的标准动作：
  ```
  powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\apply_qp_to_keil.ps1
  ```
  该脚本幂等，依次做：补 `uAC6`、补 `QV_PRJ` 宏、重建头文件路径、重建文件分组、给含中文的源文件补 UTF-8 BOM。完整流程见 `QP_INTEGRATION.md` 第十章。

### 4.2 其他工作区规则

- **临时产物不入库**：中间文件、日志、解包内容、一次性脚本放系统临时目录（例如 `%TEMP%`）；`MDK-ARM/Objects`、`Listings`、`DebugConfig`、`RTE` 等编译产物不纳入版本管理。
- **不引入新框架**：不引入 RTOS、不使用第二个事件框架；新增第三方库必须先在 `ThirdParty/README.md` 登记来源/版本/许可，并说明体积与用途。
- **不改 `Qpc/` 里的 QP 源码**，除非是为解决编译器兼容问题；每处改动必须带 `NOTE (...)` 注释，并在 `Qpc/README.md` 的改动表登记。注意 QP/C 为 GPLv3 / 商业双许可，闭源产品需另行购买商业许可。
- **新增/删除源文件后必须同步 MDK 工程分组**——跑一次 `apply_qp_to_keil.ps1` 即可（自动发现）；漏加时编译不报错，只在链接期以“符号未定义”暴露。
- git **已初始化**（2026-09-28 订正；本条此前写的是"没有初始化"）：分支 `master`，`.gitignore` 已就位（含 `MDK-ARM/Objects`、`Listings`、`*.uvguix.*`、`*.uvoptx`、`DebugConfig`、`RTE`、`Drivers/CMSIS/DSP`、PDF 插件产物 `**/*-pages/`）。
  ⚠️ **`origin` 指向公开仓库**（`https://github.com/xxz-m/stm32-nede-fw`，2026-09-28 未登录访问返回 200 确认）：**任何 push 前先确认没有凭据入包**。当前 `App/app/app_config.h` 含明文 4G 凭据且**已随 `738aef8` 推送**，处置方案见 [[docs_sensor/08-改动日志]] `#74`——在凭据轮换完成前**不要 push，也不要把它加入任何公开仓库**（与第 4.2 节的凭据纪律冲突，见该条）。
- 原则上禁止把密钥、证书、上位机私有协议原文、个人隐私数据写入仓库。
  **唯一例外（已登记）**：4G 设备云端凭据（`APP_CFG_MODEM_HOST/PORT/PRODUCT_ID/DEVICE_ID/USERNAME/PASSWORD`）经用户于 2026-09-15 明确确认，允许直接存入 `App/app/app_config.h`；明文风险已知悉，须同时遵守三条纪律：①`app_config.h` 不得进入任何公开仓库，也不得对外分发；②日志、断言、错误路径一律不打印用户名与密码（ASCII 日志同样适用）；③给第三方构建时用编译期覆盖（`-DAPP_CFG_MODEM_*` 或本地未入库头文件）替换，不改源码。此例外仅限上述 6 个宏，扩大到其他凭据须重新登记。

---

## 5. 固件架构约束

### 5.1 分层与依赖（自顶向下单向）

| 层 | 目录 | 能依赖谁 |
|---|---|---|
| App 应用层 | `App/app/` | Service、Driver、BSP、Qpc |
| 命令模型层 | `App/app_command/` | Service 中的 `svc_at.h`；不得访问 Store、QP 或硬件 |
| Service 服务层 | `App/service/` | Driver、BSP、ThirdParty |
| Driver 器件驱动层 | `App/driver/` | BSP、ThirdParty |
| 可移植组件层 | `App/component/` | 仅标准 C 与调用方注入的 Port 函数表；由 App 层调用 |
| BSP 板级支持层 | `App/bsp/` | HAL/CMSIS（**唯一允许接触 HAL 与寄存器的自研层**） |
| ThirdParty 第三方库 | `ThirdParty/` | 不依赖本工程任何一层（纯被调用方） |
| **全局配置** | `App/app/app_config.h` | **例外：所有层都可以依赖它**（只放配置宏，不放类型、不声明函数、不 include 其它项目头文件，因此不会形成依赖环） |

- **只能上层调下层**，禁止反向调用（`Drivers/`、`Core/`、`ThirdParty/` 不得回调 `App/`）。
- **`app_config.h` 是唯一的全层例外**：功能开关（日志、AT、各任务周期）、缓冲尺寸、设备标识等一律集中在这里，其它层不得散落硬编码常量。缓冲区尺寸仍由拥有者头文件对外暴露（例如 `BSP_LOG_TX_RING_SIZE` 的值取自 `APP_CFG_LOG_TX_RING_SIZE`）。
- **层内按职责拆文件**：一个 `.c` 只负责一件事，职责超过两件或超过约 200 行就拆；**BSP 按外设命名**（`bsp_uart.c`，后续 `bsp_i2c.c` / `bsp_adc.c` / `bsp_flash.c`），Service 按模块命名（`svc_frame.c`、`svc_log.c`），Driver 按器件命名（`drv_sht30.c`）。每层的聚合头（如 `bsp.h`）负责 include 各子模块头，上层只 `#include "bsp.h"` 即可。当前 BSP = `bsp.c`（QP 钩子 / 初始化 / 时间基准）+ `bsp_uart.c`（USART1 收发与异步日志发送队列）+ `bsp_ble_uart.c` / `bsp_modem_uart.c`（两个业务串口端口）+ `bsp_i2c.c`（I2C1 非阻塞主机与总线恢复）。
- 唯一例外：HAL 回调（如 `HAL_UART_RxCpltCallback()`）实现在 `App/bsp/bsp.c`，由 HAL 从中断中调用。
- 应用层与硬件无关：`App/app/` 不得 `#include` HAL 头文件、不得直接操作寄存器或引脚。
- 外设访问必须经 BSP 封装成 `BSP_xxx()`；器件语义封装在 Driver 层，协议/存储/滤波等处理放在 Service 层。
- `App/component/` 用于“一个目录即可迁移”的通信组件。组件内部可按 `core/backend` 分层，但对工程只能暴露一个公共业务头和一个 Port 头；不得包含 HAL、CMSIS、QP 或 RTOS 头，不得定义厂商 ISR，不得使用动态内存。芯片后端通过编译期宏选择，业务协议不得引用具体芯片名称。
- 新代码的落点按上表选择；不确定时先看目标层目录下的 `README.md`。

### 5.2 QP/C 与并发

- **框架**：QP/C 6.9.1 + **QV 协作式内核**（非抢占、单栈、运行到完成）。不使用 RTOS，业务并发单位是**活动对象（AO）**。
- **AO 间通信**：只通过事件（`QACTIVE_POST` / `QACTIVE_POST_X` / 时间事件）。禁止共享可变全局状态，禁止跨 AO 直接调用状态机函数。
- **状态处理函数必须尽快返回**：QV 不抢占，任何一个处理函数不返回，全系统停摆。禁止忙等、长延时、阻塞式大批量发送；耗时操作拆成“发起 → 等事件 → 继续”的多步状态。
- **中断规则**：
  - 只有 **QF-aware**（抢占优先级数值 **≥ 3**，F1 为 4 位优先级）的中断才允许调用 QP 服务。
  - ISR 只做三件事：存数据（环形缓冲）、重新装载外设、投递事件；不打印、不解析、不做业务。
  - `QF_init()` 内的 `QV_init()` 会把所有中断优先级统一设为 QF 安全值 3；此后手动设置任何优先级都不得低于 3。
- **时钟节拍**：SysTick = 100 Hz，只服务 QP（`QF_TICK_X(0U, (void *)0)`）；TIM1 = 1 kHz，服务 HAL 时基。**不得在其他位置重新配置 SysTick**。
- **空闲处理**：`QV_onIdle()` 只允许调用 `QV_CPU_SLEEP()`（必要时加 QS 跟踪发送）。**禁止在 `QV_onIdle()` 里自己写调度循环**——调度由 QV 内核负责。
- **资源静态分配**：事件队列存储用 `static` 数组，事件用静态事件 `{ (QSignal)SIG, 0U, 0U }`，不用 `malloc`；只有跨 AO 传递变长数据时才考虑 QP 事件池。
- **状态机风格**：QHsm 风格（`Q_ENTRY_SIG` / `Q_EXIT_SIG` / 自定义信号 + `Q_TRAN` / `Q_SUPER`），未处理事件一律 `status = Q_SUPER(&QHsm_top)`；初始转换函数返回 `Q_TRAN(...)`。
- **日志**：运行期输出统一用 `App/service/svc_log.h` 的 `LOG_E/W/I/D(...)`，它经 `BSP_logWrite()` 入队、由 USART1 发送中断异步搬出，调用者不等待。`APP_CFG_LOG_ENABLE = 0` 或调低 `APP_CFG_LOG_LEVEL` 时对应调用在**编译期**展开为 `((void)0)`，**调用点不写 if**。初始化横幅与命令行回显仍用阻塞的 `BSP_print*()`（内部先排空异步队列再发，避免交错）。**禁止在中断里打日志**。
- **运行期输出必须是 ASCII 英文**：串口打印与日志（`BSP_print*()` 与 `LOG_*`）里的字符串**一律用英文 ASCII**，中文只允许出现在注释与文档里。原因：调试口不应依赖上位机的编码设置——UTF-8 的中文字节在 GBK 终端下会变成乱码（实测见 `docs_sensor/08-改动日志.md` 的 `#13`）。断言消息（`Q_onError()` 的打印）同样遵守。
- **AT 指令**：调试串口的命令解析放在 Service 层（`App/service/svc_at.{h,c}`），解析状态由 `AO_Node` 持有；识别到命令后由调用方把响应写入异步队列（`LOG_WRITE()`）。新增指令在 `svc_at.c` 的命令表里追加，不在中断或 BSP 里写解析逻辑。
- **断言接线**：每个使用 QP 宏（`Q_ASSERT`、`QACTIVE_START` 等）的 `.c` 文件必须写 `Q_DEFINE_THIS_MODULE("模块名")`，并前置声明 `void Q_onAssert(char const * const module, int loc);`（实现集中在 `App/bsp/bsp.c`）。不使用断言宏的模块不要写，否则会有 `Q_this_module_` 未使用告警。
- **信号与优先级**：信号集中定义在 `App/app/app_signals.h` 并按 AO 分段（每段 32 个，段尾 `*_SIG_END`）；AO 优先级集中定义在 `App/app/app_prio.h`（`APP_<AO>_PRIO`）。两者都是应用装配信息，**不得放回 `App/bsp/`**。
- **装配点**：AO 的构造与启动由 `App/app/app.c` 的 `App_start()` 统一完成（`main.c` 的调用顺序是 `QF_init() → BSP_init() → App_start() → QF_run()`）。每个 AO 自己提供 `<Ao>_start()` 并持有事件队列存储；**BSP 不参与装配**，也不得包含任何 `App/` 头文件（中断到应用的接线用 `BSP_uartRxSetSink()` 注册回调完成）。
- **新增 AO 的标准流程**：见 `docs_sensor/05-任务说明.md#新增 AO 的步骤`；要点：在 `app_signals.h` 表尾追加信号段（`_BASE` 接上一段 `_END`）→ 类（首成员 `QActive super`）→ `<Ao>_ctor()` 与 `<Ao>_start()`（`QACTIVE_START` 的栈参数在 QV 下必须为 `(void *)0, 0U`）→ 在 `app_prio.h` 登记优先级 → 在 `App_start()` 里构造并启动。
- **内存边界**：256 KB Flash / 48 KB RAM；当前占用与分配见 `docs_sensor/03-资源分配.md`。新增功能前先评估余量。

### 5.3 第三方库

- 目录格式：`ThirdParty/<库名>/{inc,src,LICENSE,README.md}`，登记来源 URL、版本、引入日期、用途、许可。
- 只读：不修改库源码；需要适配就在 `App/driver/` 或 `App/service/` 写适配层。
- 许可：GPL 类许可与闭源产品不兼容（`Qpc/` 为 GPLv3 / 商业双许可），引入前先确认许可并留档。
- 编译：必须能在 ARM Compiler 6 下编译；未被引用的库不要加入工程分组。

---

## 6. 命名与注释

命名（与 QP 惯例保持一致）：

| 层次 / 对象 | 规则 | 示例 |
|---|---|---|
| 目录 / 文件 | 小写下划线；按层放在 `App/<层>/` | `App/driver/drv_sht30.c`、`App/service/svc_frame.c` |
| 各层函数前缀 | App 层按模块名，其余层用 `BSP_` / `DRV_` / `SVC_` | `BSP_print()`、`DRV_SHT30_Read()`、`SVC_Frame_Pack()` |
| AO 类型 / 实例 / 句柄 | 类型首字母大写 / `l_` 前缀 / `AO_` 前缀 | `Node` / `l_node` / `AO_Node` |
| 构造函数 | `<类>_ctor` | `Node_ctor()` |
| 状态处理函数 | `<类>_state_<状态名>` | `Node_state_Running()` |
| 初始转换 | `<类>_initial` | `Node_initial()` |
| 信号 | 全大写 + `_SIG`，枚举末尾放 `MAX_SIG` | `HEARTBEAT_SIG`、`RX_BYTE_SIG` |
| 静态事件 | `<模块>_<含义>Evt` | `Node_rxByteEvt` |
| 时间事件成员 | `timeEvt` | `me->timeEvt` |
| 宏 / 常量 | 全大写 | `BSP_TICKS_PER_SEC`、`BSP_RX_RING_SIZE` |
| 文件内工具函数 | `static` + 小写驼峰 | `static void printHelp(void)` |

注释：

- **语言：统一中文**（第三方源码原有注释除外）。
- 每个 `.c` / `.h` 顶部写文件头：用途、**层次定位**、目标硬件、所属内核、来源（若为第三方）。
- 函数用 Doxygen 风格 `/** @brief 中文说明 */`；参数与副作用一句话说清。
- 关键约束与坑用 `NOTE (...)` 显式标出（中断优先级、寄存器配置含义、再生成注意事项、编码陷阱）。
- 解释“为什么这么做”，而不是复述语句做了什么。
- 时间参数用 `BSP_TICKS_PER_SEC` 表达（如 `BSP_TICKS_PER_SEC/2`），不要写裸数字。
- 寄存器配置的魔法数字必须注释含义（例：`RCC->CFGR = 0x001D0402;` → PLLMUL=x9 / PLLSRC=HSE / PPRE1=/2）。
- 修改第三方文件时，注释里标 `NOTE (..., 为接入本工程新增)`，并在对应 `README.md` 登记。

---

## 7. 文档维护

- **改代码就要改文档**，同步范围：
  - 分层 / 目录结构 / 新增一层 → `docs_sensor/02-程序框架.md`
  - 引脚 / 时钟 / 中断 / 内存变化 → `docs_sensor/03-资源分配.md`
  - AO、事件、优先级、节拍变化 → `docs_sensor/04-任务调度.md`
  - 状态机、命令行为变化 → `docs_sensor/05-任务说明.md`
  - 串口收发格式变化 → `docs_sensor/06-协议格式.md`
  - 工程结构 / 接入方式 / 再生成流程变化 → `sensor-node-fw/QP_INTEGRATION.md`
  - 新增第三方库 / 改动库 → `sensor-node-fw/ThirdParty/README.md`
  - `Qpc/` 内的改动 → `sensor-node-fw/Qpc/README.md`
  - 层的职责与边界 → 该层目录下的 `README.md`
  - **任何有意义的改动（代码、配置、脚本、约束） → `docs_sensor/08-改动日志.md`（格式见第 9 节）**
  - 接口形态 / 错误码 / 命名与单位约定 → `docs_sensor/07-接口约定.md`
- 文档风格遵循 `docs_sensor/00-索引.md` 的维护约定：frontmatter（`status`、`owner`、`updated`）、标识符用反引号、图用 Mermaid、内部引用用 wikilink。
- 每次改动都要更新对应文档的 `updated` 字段。
- 文档要写“**怎么验证**”，命令必须可直接复制运行；不要只写“怎么做”。
- 引用第三方代码/文档时注明来源与许可（QP/C 6.9.1、ST HAL、CMSIS）。
- 本文件（`AGENTS.md`）是约束的唯一入口：约束变化先改这里，再改代码。

---

## 8. 验证要求

分级要求，缺一不可：

| 级别 | 触发条件 | 必须做的事 |
|---|---|---|
| L1 编译 | 任何代码改动 | Keil（ARM Compiler 6）全量编译 **0 error**；报告 error/warning 数量与 Flash/RAM 占用 |
| L2 无 Keil 时的烟测 | 无 Keil 环境或提交前快速自检 | 运行 `powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\smoke_build.ps1`，要求 errors = 0 且链接通过；记录镜像大小。**这是替代手段，不能取代 L1** |
| L3 工程完整性 | 新增/删除/移动文件、改 include/宏、CubeMX 再生成后 | 运行 `App/tools/apply_qp_to_keil.ps1`，确认输出为“无需修改”或已补齐、`缺失 = 0`，且含中文源文件都已补 BOM |
| L4 中断审计 | 涉及中断、优先级、时基 | 复核所有 QF-aware 中断抢占优先级 ≥ 3，并核对 `docs_sensor/03-资源分配.md` |
| L5 硬件在环 | 有板子时，发布/里程碑前 | 串口 115200：可见启动横幅与 `actors` 行、`AT` 回 `OK`、`AT+SN?` 回 `+SN:"<值>"`、1 Hz 采样行与 2 s 心跳行持续出现；涉及参数存储时再验 `AT+SN="<新值>"` → `store: saved ...` → 断电重启后仍是新值；记录实测结果与日期。**可直接用 `App/tools/at_smoke.ps1` 跑（默认只读）**，判读方法见 `docs_sensor/06-协议格式.md#怎么验证` |

硬性禁止：

- 未编译就声称“完成/通过”。
- 把 warning 当成通过而不说明。
- 没有硬件却声称“已验证运行”。
- 报告验证结论时不给出实际命令与实际输出摘要。

---

## 9. 改动日志

**位置**：`docs_sensor/08-改动日志.md`。它是文档资料的正式组成部分（挂在 `docs_sensor/00-索引.md` 里），不是可选的附录。

**硬性要求**：

- 每一次对仓库有意义的改动（代码、`.ioc` / Keil 工程配置、脚本、约束、文档规则）都**必须在同一次改动内**往日志追加条目，**不允许事后补记**。
- 排序：日期倒序；同一天内用 `#序号` 倒序（新条目在上）。
- 每条必须写全五个字段：**类型**（新增 / 修复 / 重构 / 配置 / 文档）、**变更**（文件或目录）、**原因**（为什么要改）、**验证**（做了哪一级 L1~L5、命令、结果数字）、**遗留**（未完成事项与下一步）。
- “验证”字段必须写实际结果（例如 `82 文件 0 error / 0 warning，链接通过，Flash 44637 B / RAM 528 B`），不许只写“已测试”“应该没问题”。
- 本轮改坏又修好的问题、临时绕过的坑，写进对应条目的“修复”或“遗留”，避免重复踩坑。
- 新增文档 / 目录 / 层级时，同时更新 `docs_sensor/00-索引.md`（阅读顺序、文档清单、常用跳转）与本文档第 3 节的仓库结构。
- 里程碑（可上板运行的版本）另起一条，标注版本号与 L5 实测结果。

---

## 10. 冲突处理：需求与文档冲突时先提问

**规则**：当用户的需求 / 指令与仓库文档（`docs_sensor/*`、`AGENTS.md`、`QP_INTEGRATION.md`、各层 `README.md`）不一致时，**先向用户提问确认，不要自行改动**。

两类冲突要分开处理，不要混为一谈：

| 冲突类型 | 典型例子 | 处理方式 |
|---|---|---|
| 事实性偏差（文档写错了） | 文档里还写着 `App/inc/` 而代码已是 `App/app/`；引脚表与 `.ioc` 不符；状态码清单漏了新值 | 按第 2 节规则**直接以代码 / `.ioc` 为准修正文档**，并在改动日志里说明 |
| 意图性冲突（需求与文档的设计约定不同） | 要求把 HAL 时基改回 SysTick，而第 5.2 节规定 SysTick 只服务 QP；要求跨层直接调用，而第 5.1 节规定依赖单向向下；要求改协议帧格式，而 [[06-协议格式]] 已固化 | **停下来提问**：先讲清冲突点与影响，等用户选择后再动手 |

提问必须包含四件事：

1. **冲突点**：哪条需求与哪份文档的哪一节冲突，附文件路径与章节名
2. **影响面**：涉及哪些层 / 文件，是否需要动 `.ioc`、Keil 工程、协议或 AGENTS.md 本身
3. **备选方案**：至少两个（含「按文档」与「按需求」两版），分别写清代价
4. **建议**：证据倾向某一方时说明理由，但**不要代替用户决定**

禁止的做法：

- 自行判断「用户说得对」就改代码、文档不动
- 自行判断「文档写得对」就忽略用户需求
- 两边各改一点，把冲突抹平成第三种约定
- 不提问就改，事后在日志里补一句「与文档不一致」

**用户确认之后**：按结论落地，并同步更新冲突的文档（或按用户决定修改文档本身），日志里写明「本次按用户确认，改以 X 为准」，附上提问与结论摘要。

---

## 附：常用命令

```powershell
# 补回 Keil 工程引用 + 统一中文源码编码（CubeMX 再生成后执行，幂等）
powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\apply_qp_to_keil.ps1

# 无 Keil 时的全量编译 + 链接烟测
powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\smoke_build.ps1

# 上板跑一遍 AT 命令与参数存储检查（L5 硬件在环；默认只读，跑前先关掉串口助手）
powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\at_smoke.ps1 -ListPorts
powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\at_smoke.ps1 -Port COM5
```
