# QP/C 6.9.1（QV 内核）接入说明

> **2026-09-20 业务串口对调 + 实例参数化**：按实机接线，**BLE 改走 `USART2`（PA2/PA3）、4G 模组走 `USART3`（PB10/PB11）**。两个业务端口的实例不再硬编码在 BSP 里，改为装配时注入：`APP_CFG_BLE_UART_ID` / `APP_CFG_MODEM_UART_ID` → `BSP_bleUartInit(port, id)` / `BSP_modemUartInit(port, id)`，HAL 公共回调按 `huart->Instance` 翻成 `bsp_uart_id_t` 后分派。另：补上 **USART1 接收错误恢复**（原先 ORE 后不再重装，会静默失收），并把两个模组的电源使能配成推挽输出拉高——BLE 的 `VG6_EN`(PB1) 与 4G 的 `MN316_EN`(PB9)；4G 的复位脚 `NB_RESET`(PB8) 按用户决定保持悬空。4G 侧随后开回（**MN316**，AT 口 9600），即本轮发布态为 BLE + 4G 双开。
>
> **2026-09-14 通信组件接入**：工程新增 `App/component/` 与 `AO_Ble`。`apply_qp_to_keil.ps1` 会递归发现 Component 的源码和头目录并建立 `Component` 分组；当时 VG6328 配在 USART3/PB10/PB11，IRQ 优先级 5（**该接线已被 2026-09-20 的对调取代**）。新增文件或 CubeMX 再生成后仍按第十章运行恢复脚本。

本工程是在 **STM32CubeMX 生成的基础工程**（`sensor-node-fw.ioc`，MCU = STM32F103RCT6，LQFP64）之上，接入 **QP/C 6.9.1** 实时事件框架（**QV 协作式内核**）的集成示例。本文件记录接入了什么、怎么接的、以及你打开 Keil 之后需要确认什么。

## 一、采用的方案（按你的选择）

| 项 | 选择 |
|---|---|
| 内核 | **QV**（非抢占、协作式、单栈） |
| SysTick | **方案 A**：HAL 时基挪到 TIM1，SysTick 只给 QP 做时钟节拍（100 Hz） |
| 编译器 | **ARM Compiler 6 / armclang**（工程里已置 `uAC6=1`） |
| 板载外设 | USART1 调试（PA9/PA10，115200）+ BLE `USART2`（PA2/PA3，115200）+ 4G `USART3`（PB10/PB11，MN316 档 9600）；BLE 电源使能 `VG6_EN`(PB1)、4G 电源使能 `MN316_EN`(PB9)、4G 复位 `NB_RESET`(PB8，悬空) |

## 二、目录结构（哪些是 CubeMX 管的，哪些不是）

```
sensor-node-fw/
├── sensor-node-fw.ioc        <- CubeMX 工程文件（本次只改了 USART1 中断优先级：0 -> 5）
├── Core/                     <- CubeMX 管理（只在 USER CODE 区里加了 QP 代码）
│   ├── Inc/  main.h, usart.h, stm32f1xx_hal_conf.h, ...
│   └── Src/  main.c, stm32f1xx_it.c, usart.c, stm32f1xx_hal_timebase_tim.c, ...
├── Drivers/                  <- CubeMX 管理（STM32F1 HAL + CMSIS，未改动）
├── Qpc/                      <- QP/C 6.9.1 源码（platform + QV 端口），见 Qpc/README.md
├── App/                      <- 自研代码，按分层建目录（CubeMX 不管理）
│   ├── app/      应用层：装配点、全局配置、五个 AO（node/sensor/heartbeat/ble/modem）、统一命令执行
│   ├── app_command/ 可移植命令模型：来源、SvcAtCmd、关联 ID、统一结果
│   ├── service/  服务层：AT、异步日志、滤波、CRC-16 组帧/解析
│   ├── driver/   器件驱动层：传感器统一接口（当前为仿真数据源）
│   ├── component/ 可移植通信组件：ble/（VG6328）与 modem/（三后端 + AT 引擎）
│   ├── bsp/      板级支持层：QP 钩子/节拍 + USART1 收发/异步队列 + BLE/4G 端口（实例注入）
│   └── tools/    工程与验证脚本：apply_qp_to_keil.ps1、smoke_build.ps1
├── ThirdParty/               <- 第三方库（<库名>/{inc,src,LICENSE,README.md}），见其 README.md
└── MDK-ARM/
    └── sensor-node-fw.uvprojx <- 文件组 / 宏 / include / uAC6 由 App/tools/apply_qp_to_keil.ps1 自动重建
```

**重要**：`App/` 和 `Qpc/` 里的代码都是新增文件，CubeMX 重新生成代码时不会碰它们；`Core/` 里只在 `USER CODE BEGIN/END` 区里加了代码，重新生成也会保留。

## 三、接入点（QP 是怎么跑起来的）

1. `Core/Src/main.c`
   - `USER CODE BEGIN 2`：`QF_init(); BSP_init(); App_start();`
   - `USER CODE BEGIN 3`：`(void)QF_run();` —— QV 内核的事件循环，不返回。
2. `Core/Src/stm32f1xx_it.c`
   - `SysTick_Handler()` 的 `USER CODE BEGIN SysTick_IRQn 1`：`QF_TICK_X(0U, (void *)0);`
3. `App/bsp/bsp.c`
   - `BSP_init()`：用 `HAL_SYSTICK_Config(SystemCoreClock / 100)` 配置 **100 Hz** 的 QP 节拍（因为 HAL 时基已经挪到 TIM1，HAL_Init 不会启动 SysTick）；启动 UART 中断接收；打印启动横幅。
   - 不参与 AO 装配；USART1 接收通过 `BSP_uartRxSetSink()` 注册回调。
   - QP 钩子：`QF_onStartup()`、`QF_onCleanup()`、`QV_onIdle()`（里面只有 `QV_CPU_SLEEP()`，即 PRIMASK → WFI → 开中断）、`Q_onError()` / `Q_onAssert()`。
   - `HAL_UART_RxCpltCallback()` 位于 `bsp_uart.c`：重新武装接收并调用已注册的 sink；`Node_uartRxIsrSink()` 再把字节写入环形缓冲并投递事件。同文件的 `HAL_UART_TxCpltCallback()` / `HAL_UART_ErrorCallback()` 按 `huart->Instance` 分派给 BLE / 4G 端口，并**已补上 USART1 出错后的接收恢复**（2026-09-20）。
4. `App/app/node.c`
   - `Node` 是 AT-only 单状态 AO：收到 `NODE_RX_BYTE_SIG` 后抽干环形缓冲，解析为 `SvcAtCmd`，构造串口来源的 `AppCommandRequest`，调用 `App_Command_Dispatch()`，再把 `AppCommandResult.raw` 写入异步日志队列。
5. `App/app/app.c`
   - `App_start()` 注册 UART sink，构造并启动各 AO（本轮 node/sensor/heartbeat/ble 四个，modem 随 `APP_CFG_MODEM_ENABLE=0` 消失）；随后投递 `SYS_SENSOR_START_SIG`，让仿真采样上电自动运行。

## 四、中断优先级规则（务必记住）

- QP 在 Cortex-M3 上用 **BASEPRI**（`QF_BASEPRI = 0x3F`）做临界区，F1 有 4 位优先级，因此 **`QF_AWARE_ISR_CMSIS_PRI = 3`**：抢占优先级数值 **≥ 3** 的中断才能被 QP 屏蔽，也只有这些中断**允许调用 QP API（发事件等）**。
- CubeMX 里 NVIC 分组已是 `NVIC_PRIORITYGROUP_4`（4 位抢占 / 0 位子优先级），正确。
- USART1 的中断优先级已从 0 改成 **5**（`.ioc` 与 `usart.c` 同步修改），这样 UART 中断发事件是合法的。
- 另外，`QF_init()` 里的 `QV_init()` 会在启动时**把所有中断优先级统一刷成 3**（官方"安全值"）。之后你若手动改优先级，请保持 **≥ 3**。

> [!important] I2C1 中断不走 `.ioc`（2026-09-27 起）
> 参数存储用的 `I2C1_EV_IRQn` / `I2C1_ER_IRQn`（抢占优先级 **5**）**没有**写进 `.ioc`，改由
> `App/bsp/bsp_i2c.c` 负责：`BSP_i2cInit()` 调 `HAL_NVIC_SetPriority/EnableIRQ()`，
> 并在同一文件里给出 `I2C1_EV_IRQHandler` / `I2C1_ER_IRQHandler` 的强定义（覆盖启动文件的 `[WEAK]` 向量）。
>
> 这样做的原因：**不想为了一个外设再多一次「改 `.ioc` → GENERATE CODE → 跑恢复脚本」的流程**。
> `App/bsp/` 不受 CubeMX 管理，所以这份配置同样不会被再生成覆盖，也不会与生成代码重复定义。
>
> ⚠ 两条纪律：① 若将来在 CubeMX 里勾选了 I2C1 的 Event/Error 中断，**必须删掉 `bsp_i2c.c` 里的
> 两个 handler**，否则链接期 `multiply defined`；② `BSP_i2cInit()` 必须在 `QF_init()` 之后运行
> （当前由 `BSP_init()` 调用），否则优先级会被 `QV_init()` 改成 3——仍可用，但与你看到的不一致。

## 五、时钟与节拍

| 时钟源 | 用途 | 频率 |
|---|---|---|
| SysTick | QP 时钟节拍（`QF_TICK_X`，tick rate 0） | 100 Hz（`BSP_TICKS_PER_SEC`） |
| TIM1 | HAL 时基（`HAL_IncTick`，`HAL_Delay`/超时用） | 1 kHz |

注意：因为 TIM1 的 1 ms 中断一直在跑，`QV_onIdle()` 里的 WFI 最多只能睡 1 ms 就会被唤醒。想做深睡眠（STOP 模式 + 关 TIM1）需要额外的低功耗设计，可以后面再做。

## 六、为 AC6 对 QP/C 6.9.1 端口做的兼容性修改

QP/C 6.9.1 的 `arm-cm` 端口是 ARM Compiler 5 时代的代码，直接用 armclang 编译会失败。共改了 4 处（全部在 `Qpc/port/qv/inc/` 下，且有注释标明）：

| 文件 | 原来（AC5） | 现在（AC5/AC6/GCC 通用） |
|---|---|---|
| `qep_port.h` | `#define Q_NORETURN __declspec(noreturn) void` | AC6/GCC 用 `__attribute__((noreturn))`，AC5 仍用 `__declspec` |
| `qf_port.h` | `register unsigned volatile __regBasePri __asm("basepri")` | 改用 CMSIS 的 `__get_BASEPRI()` / `__set_BASEPRI()` |
| `qf_port.h` | `__asm("isb")` | AC6/GCC 用 `__asm volatile ("isb")` |
| `qf_port.h` | `QF_LOG2` 用 `__clz()` | AC6/GCC 用 `__builtin_clz()` |
| `qv_port.h` | `__wfi()`、`__asm("dsb")` | 改用 CMSIS `__WFI()` / `__asm volatile ("dsb")` |

另外 `qf_port.h` 顶部加了 `#include "cmsis_compiler.h"`，让端口不再依赖 AC5 的寄存器变量语法。

## 七、验证情况（重要）

**已验证**：本机没找到 Keil（未在常见路径发现 `UV4.exe`/`armclang`），所以用 STM32CubeCLT 自带的 `arm-none-eabi-gcc` 做了完整的编译 + 链接烟测：

- 82 个源文件（Core + App + HAL + QP platform/QV 端口）全部编译通过：**0 error / 0 warning**；
- 完整链接成功（所有符号都能解析，说明 QP 钩子、HAL 回调、中断入口都接全了）；
- 镜像规模：Flash ≈ **44.7 KB** / 256 KB，RAM ≈ **0.5 KB** / 48 KB。

> 注意：烟测用的 GCC 与你要用的 AC6 不是同一个编译器，但它能验证语法、类型、符号接线，也顺带验证了上面那批 AC6 兼容改动（因为这些改动同时兼容 GCC）。**最终请在 Keil 里用 AC6 编译确认一遍。**

## 八、打开 Keil 后的检查清单

1. 打开 `MDK-ARM/sensor-node-fw.uvprojx`。工程已置 `uAC6=1`，若你的 MDK 里 AC6 版本不同，µVision 会提示选择编译器版本，选 AC6 即可。
2. **启动文件**：工程用的是 CubeMX 生成的 `startup_stm32f103xe.s`（armasm 语法，由 Arm 的传统汇编器处理）。如果 MDK 报启动文件汇编错误，说明你的安装缺少传统 armasm，需要换用 armclang 语法的启动文件（或安装 AC5 兼容组件）。
3. **Flash 下载算法**：CubeMX 生成的工程里 `<FlashDriverDll>` 是空的，第一次打开请到 `Options for Target → Debug/Utilities → Settings → Flash Download` 里确认有 `STM32F10x_512`（256 KB 档）。
4. **宏定义**：`C/C++ → Define` 应为 `USE_HAL_DRIVER,STM32F103xE,QV_PRJ`。
5. **头文件路径**：应包含 `../App/app`、`../App/bsp`、`../Qpc/platform/inc`、`../Qpc/port/qv/inc`（脚本按磁盘实际目录自动重建，通常无需手改）。
6. 编译后下载运行，串口 115200 应看到标准横幅、`actors` 与 `boot ok`，默认无1Hz采样与2s心跳；4G首次上线或分钟周期到期先采后发，模组确认后打印`cloud> sent`；发送`AT`加CR/LF应收到`OK`，按键还需验证120s蓝牙广播窗口。

## 九、后续可以继续加的东西

- **加业务 AO**：在 `App/app/` 新增 AO，在 `app_signals.h` / `app_prio.h` 登记，并由 `App_start()` 装配；队列归 AO 自己持有。
- **QS 软件跟踪**：把 `Qpc/spy` 加进工程（需要 curl `Q_SPY` 宏 + `qs_port.h`），能让 QView/QSPY 直接看到状态机跳转；当前版本没开，串口被应用自己用了。
- **深睡眠**：STOP 模式 + 关掉 TIM1 时基 + RTC/EXTI 唤醒，配合 `QV_onIdle()` 实现电池供电节点。
- **切换到 QK/QXK**：仓库里带了 `Qpc/platform/src/qk.c`、`qxk*.c`，但它们是 AC5 内联汇编写法（`__asm void PendSV_Handler(void)` 等），**AC6 下不能直接编译**；要换内核建议改用官方 QP/C 7.x/8.x 的 armclang 端口。

## 十、CubeMX 重新生成代码后怎么办（重要）

**设计原则：所有手写代码都放在 CubeMX 不管理的地方。** CubeMX 只会重写它自己生成的 `Core/`、`Drivers/` 和 `MDK-ARM/<工程>.uvprojx`，永远不会删 `App/`（应用层）和 `Qpc/`（QP/C 源码）这两个文件夹。

| 改动 | 位置 | CubeMX 再生成后 |
|---|---|---|
| `QF_init / BSP_init / App_start / QF_run` | `Core/Src/main.c` 的 USER CODE 区 | ✅ 保留（`.ioc` 里 `KeepUserCode=true`） |
| `QF_TICK_X()` | `Core/Src/stm32f1xx_it.c` 的 USER CODE 区 | ✅ 保留 |
| USART1 中断优先级 = 5 | **`.ioc` 配置**（`usart.c` 会被重写） | ✅ 保留（`.ioc` 已存 5，再生成时自动写回 5） |
| 业务串口的**线速率** | `App/bsp/bsp_ble_uart.c` / `bsp_modem_uart.c` 的 `*UartInit()` | ✅ 保留（`App/` 不受管理）——**故意不写 `.ioc`**：实例是装配信息、会对调，速率必须由占用该实例的端口在运行时写入。再生成后 `usart.c` 里的值（USART2/USART3 都是 115200 默认）**不代表最终生效值**，以启动日志打印的实际值为准 |
| HAL 时基 = TIM1、NVIC 分组 4 | `.ioc` 配置 | ✅ 保留 |
| 全部应用代码（bsp、node、事件、状态机） | `App/` | ✅ CubeMX 不碰这两个文件夹 |
| QP/C 6.9.1 源码与 QV 端口 | `Qpc/` | ✅ CubeMX 不碰 |
| 调试器、Flash 下载算法 | `MDK-ARM/<工程>.uvoptx`（µVision 维护） | ✅ 不受影响（你的工程里已经是 ST-Link + `STM32F10x_512`） |
| **Keil 工程里的文件组 / `QV_PRJ` 宏 / 三条 include 路径 / `uAC6`** | `MDK-ARM/<工程>.uvprojx` | ❌ **会被重置**（ST 社区已确认 CubeMX 会丢掉手工添加的文件与分组） |

所以再生成之后，**只差把工程引用补回去**这一步，已经准备好幂等脚本：

```
powershell -NoProfile -ExecutionPolicy Bypass -File App\tools\apply_qp_to_keil.ps1
```

它会补回：`<uAC6>1</uAC6>`、`QV_PRJ` 宏、`../App/app`、`../App/bsp`、`../Qpc/platform/inc`、`../Qpc/port/qv/inc` 等头文件路径，以及 `App` / `Service` / `Driver` / `BSP` / `QP/C/platform` / `QP/C/port` 等文件组（路径与分组都按磁盘实际文件**自动发现**，所以新增或移动 `.c`/`.h` 不需要改脚本），最后给含中文的源文件补 UTF-8 BOM、校验工程 XML 与所有文件引用的存在性。**重复运行安全**（工程已是最新时输出“无需修改”）。已用“模拟一次 CubeMX 再生成（把这些内容全部删掉）→ 跑脚本”的方式验证过，全部恢复。

推荐操作顺序（避免改动被覆盖）：

1. CubeMX 里点 `GENERATE CODE`；
2. 如果 µVision 弹"工程文件已被外部修改，是否重新加载"，先选 **否**，并把工程关掉；
3. 运行上面的脚本；
4. 重新打开 `MDK-ARM\sensor-node-fw.uvprojx` 编译。

另外我在 `MDK-ARM/` 下放了一个**空文件 `project.uvmpw`**：ST 社区有人反馈，存在这个文件时 CubeMX 再生成不会清掉 Keil 工程里手工添加的文件/分组。属于社区经验（不是 ST 官方文档承诺），值得一试——生效的话你连脚本都不用跑，不生效就用脚本兜底。**无论哪种情况，`App/` 和 `Qpc/` 里的代码都不会丢。**

如果你接下来还要加第 2、3 条约束，直接说，我按同样的原则处理：能靠 `.ioc` 配置的就不改生成代码，必须改生成文件的只动 USER CODE 区，其余一律放进 `App/`。

## 电池 ADC 再生成说明（2026-10-12）

PC1 默认模拟输入（高阻）、PC0=ADC1_IN10 与 ADCPCLK2_DIV6=12MHz 已写入 `.ioc`。当前运行期 ADC/开关由 `App/bsp/bsp_adc.*` 初始化，未手改生成的 Core 或 HAL，也不依赖 HAL_ADC 模块开关。内部 VREFINT 通道由 BSP 按需开启，完成作业后关闭 ADC/VREFINT。
PB15 按键已登记为 `GPIO_Input`、`GPIO_NOPULL`（外部硬件上拉）。运行期由 `bsp_gpio.*` 初始化，Driver 仅做消抖；App 复用 Modem 的周期事件扫描并映射业务信号，未新增 Core 修改或 EXTI。新增源文件后运行工程恢复脚本，再执行 `button_test.ps1` 与 Keil L1。
CubeMX 再生成时可能生成 ADC 初始化文件；保留生成流程，运行期仍由 BSP 在启动后登记并管理电池 ADC，禁止同时从其他 AO 发起 ADC1 事务。跑 `apply_qp_to_keil.ps1` 自动同步新增 Driver/BSP 文件及中文 BOM，检查 XML/缺失引用，再做 L1 编译和 L5 测量。

恢复脚本写回前统一 `.uvprojx` 为 CRLF；即使原工程已有混合换行也会修正。验证时重复执行两遍，第二遍应显示「无需修改」，再检查 XML 与引用缺失数。
