---
status: 进行中
owner: 待填
updated: 2026-10-14
---

# 器件驱动层（Driver）

层次位置：App 应用层 → Service 服务层 → **Driver 器件驱动层** → BSP → HAL/CMSIS

本层传感器实现为 `drv_scl3300.{h,c}`（真实 SCL3300-D01），公开接口保留 `DRV_Sensor_*`
前缀；原仿真源 `drv_sensor.{h,c}` 已删除。存储器相关实现为 `drv_at24c04.*`、
`drv_storage.*`、`drv_w25q32.*` 和 `drv_w25q32_io.c`，当前参数存储使用 W25Q32。

## 职责

把具体器件抽象成上层看得懂的语义接口，屏蔽总线与寄存器细节：

- 传感器：封装寄存器读写和器件状态；当前 SCL3300 驱动同时完成数据手册规定的工程量换算，业务滤波和标定不在本层
- 存储器：EEPROM / Flash 参数区读写
- 显示屏、执行器等其他器件的统一访问接口

目标效果：上层通过 `DRV_Sensor_StartSample()` / `Process()` / `GetSample()` 取得工程量，
不直接处理 SPI 命令、DMA 或片选。

## SCL3300 底层阅读顺序

需要从 SPI 基础开始时，先读 [[07-接口约定#SPI 通用知识手册与 SCL3300 驱动实例]]，再按下表对照源码。

| 顺序 | 文件 / 函数 | 重点 |
|---|---|---|
| 1 | `drv_scl3300.h` | 结果单位、接口调用顺序、成功/失败结果如何消费 |
| 2 | `drv_scl3300.c` 的 `DRV_Sensor_Process()` | 初始化、采样、恢复三条路径如何选择 |
| 3 | `SCL_InitStep()` | 软件复位、等待、模式设置、角度使能、STATUS、WHOAMI |
| 4 | `SCL_JobStep()` | 7 个量各发两帧，第一帧提交、第二帧收数据 |
| 5 | `SCL_BuildFrame()` / `SCL_RxData()` | 固定命令尾部的表示、字节顺序、CRC 和 RS |
| 6 | `SCL_Submit()` / `SCL_Collect()` | 发起与收取分开；收取才释放 SPI 所有权 |
| 7 | `SCL_RecoverStep()` | 失败样本不提交，STATUS 读清不成功时延迟重初始化 |
| 8 | `../bsp/bsp_spi.c` / `bsp_spi_transfer.c` | 设备片选和分频、DMA 启动、完成检查及总线所有权 |
| 9 | `../app/sensor.c` | AO 用周期事件驱动底层，取样后才进行滤波和组帧 |

正常调用链为：`Init()` 重置软件状态 → 周期调用 `BSP_spiProcess()`、`DRV_Sensor_Process()`
直到 `IsReady()` → `StartSample()` → 继续周期推进 → `IsDone()` → `GetSample()`。
`Init()` 返回成功只是软件初始化完成；`IsDone()` 为真也可能是失败，必须检查 `GetSample()` 返回值。

器件响应滞后一条命令，因此 CRC 正确也可能是上一寄存器的数据；需要同时理解 `point_first`
与命令发送顺序。`init` 管器件就绪和恢复，`job` 管当前结果，两者不能混为一个状态。
`status_valid` 只表示已取得 STATUS，不保证无故障，也不表示本次采样新读了 STATUS。

角度按16位补码先符号扩展，再以 `signed_raw * 9000 / 16384` 换算百分度（Murata手册§6.1.3）。2026-10-14修复负小角度被解释成约360°、再经四点平均产生90°假跳变的问题；符号处理必须先于Service滤波。零点标定仍未实现。
怎么验证：`powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw/App/tools/scl3300_test.ps1`，覆盖65536个原始值、±180°编码边界、手册正角度向量、实际错帧SPI及四点批采样的负值/过零/交替正负；主机通过不代替上板L5。
温度是片内温度，输出 0.01°C；加速度单位 mg 表示千分之一 g，不能按质量的毫克理解。

### 怎么验证注释对应的路径

在仓库根目录可复制执行：

```powershell
rg -n 'SENSOR_PROCESS_SIG|BSP_spiProcess|DRV_Sensor_' sensor-node-fw/App/app/sensor.c
rg -n 'SCL_InitStep|SCL_JobStep|SCL_RecoverStep|SCL_Submit|SCL_Collect' sensor-node-fw/App/driver/drv_scl3300.c
rg -n 'l_owned|l_done|BSP_spiResult|BSP_spiProcess' sensor-node-fw/App/bsp/bsp_spi_transfer.c
powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw/App/tools/smoke_build.ps1
```

前三条用于对照实际调用和所有权释放点，最后一条是 L2 编译烟测；其通过不能代替 Keil L1
或上板 L5。硬件验证及历史故障见 [[08-改动日志]] 的 #92～#103。

## 电池分压驱动

`drv_battery.*` 通过 `BSP_adc*` 与 `BSP_batteryGateSet()` 访问硬件，不直接包含 HAL。
`Init → StartSample → 周期 Process → IsDone → GetSample`；失败终态也必须消费，取消用 `Cancel()`。
PC1 拉低开启，等待至少 5 ms 后读 PC0 四次，释放 PC1，再读内部参考四次；成功、超时和取消都释放开关并关闭 ADC/VREFINT。
当前 `APP_CFG_BATTERY_GATE_HOLD_LOW=0`，恢复上述正常流程。排障时可设为 1，让 BSP 忽略释放请求、始终保持 PC1 开漏低以测量 PC0；测试模式不改变 Driver 的采样时序和结果判断。
电压以 mV 返回，基于 VREFINT 典型值估算，不输出电量百分比；量程和平均次数集中在 `app_config.h`。
失败可用 `DRV_Battery_GetDiagnostic()` 查具体原因、通道、ADC 状态、采样次数及原始值；诊断保留至下次作业，不能代替成功快照。日志由 AO_Sensor 输出，Driver 不反向依赖日志服务。详见 [[07-接口约定#电池电压接口]]。
主机验证：`powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw/App/tools/battery_test.ps1`（仓库根目录）。
现板 PC1 的 4.2 V 上拉风险及硬件验证要求见 [[03-资源分配#电池采样与现板限制]]。

## 依赖规则

`drv_button.*` 只做通用按下消抖：配置10ms 稳定后返回一次 `true`，长按不重复，上电按住需先释放。实例由调用方持有，毫秒计数回绕不影响期限。驱动没有 QP 信号、4G 组件或业务回调，事件映射在 App 层；BSP 只读 PB15 逻辑电平。验证：`powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw/App/tools/button_test.ps1`。

- 只允许调用 `App/bsp/` 里的接口（如后续新增的 `BSP_i2c_*` / `BSP_spi_*` / `BSP_uart_*`）
- 可以调用 `ThirdParty/` 里的算法或协议库
- **不允许**直接 `#include` HAL 头文件、不允许直接配置寄存器或引脚 —— 这些属于 BSP
- 不允许调用 `App/service/`、`App/app/`（依赖只能向下）

## 命名与实现约定

| 项 | 约定 |
|---|---|
| 文件 | `drv_<器件>.c` / `drv_<器件>.h`，例如 `drv_sht30.c` |
| 函数 | `DRV_<器件>_Init()`、`DRV_<器件>_Read()`、`DRV_<器件>_Write()` |
| 返回值 | `drv_status_t`：`DRV_OK` / `DRV_ERR_PARAM` / `DRV_ERR_BUS` / `DRV_ERR_TIMEOUT` / `DRV_ERR_NODEV` / `DRV_ERR_BUSY` |
| 阻塞 | 允许短时间的总线等待；超过毫秒级的等待必须拆成"发起 → 事件驱动 → 继续"的异步接口，不能阻塞 AO |

## 落地检查清单

新增一个器件驱动时：

1. 先在 `App/bsp/` 里把所需总线封装成 `BSP_xxx()` 接口（禁止在驱动里直接碰 HAL）
2. 在 `App/driver/` 新建 `drv_<器件>.c/h`，实现 `Init/Read/Write` 与统一错误码
3. 用到的总线、引脚、从机地址同步登记到 `docs_sensor/03-资源分配.md`
4. 运行 `App/tools/apply_qp_to_keil.ps1` 把新文件加入 Keil 分组（脚本会自动发现）
5. 同步 `docs_sensor/02-程序框架.md` 与相关文档的 `updated`

当前业务周期由App请求式批采样统一管理，Driver不维护分钟上传或蓝牙广播时间。PB15按下结果由App映射为两个业务事件，Driver无需改动。旧1Hz采样和2s心跳验证口径已由用户确认替换，见[[05-任务说明]]。
