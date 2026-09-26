# sensor-node-fw

STM32F103RCT6 传感器节点固件工程，基于 STM32CubeMX/HAL 与 QP/C 6.9.1 的 QV 协作式内核。

> 当前版本是**可运行的固件骨架**，不是最终产品固件。传感器目前使用仿真数据源，真实传感器接入、参数持久化、Bootloader/OTA 等功能仍待实现。

## 项目概览

- MCU：STM32F103RCT6，Cortex-M3，72 MHz
- 资源：256 KB Flash、48 KB RAM
- 框架：QP/C 6.9.1，QV 协作式内核，单栈运行
- 配置：STM32CubeMX 6.15.0，工程配置位于 `sensor-node-fw/sensor-node-fw.ioc`
- 编译：Keil MDK-ARM + ARM Compiler 6；无 Keil 时可使用 GCC 烟测脚本
- 通信：USART1 调试/AT，USART2 BLE（VG6328），USART3 4G（MN316）

## 目录导航

```text
.
├── README.md                       本文件：项目入口与快速开始
├── AGENTS.md                      工作区约束与开发规范
├── docs_sensor/                   工程文档（从 00-索引.md 开始）
├── 2.模块数据手册及资料/           模组与传感器参考资料
├── ml307c-dtu-test-report/        4G/DTU 测试报告与串口日志
└── sensor-node-fw/                STM32 固件主工程
    ├── sensor-node-fw.ioc         CubeMX 唯一配置源
    ├── App/                       自研应用代码
    │   ├── app/                   AO、装配点与全局配置
    │   ├── service/               AT、日志、滤波、帧协议
    │   ├── driver/                器件驱动抽象
    │   ├── component/             可移植 BLE/4G 通信组件
    │   ├── bsp/                   板级支持与串口封装
    │   └── tools/                 工程修补与验证脚本
    ├── Core/                      CubeMX 生成代码
    ├── Drivers/                   STM32 HAL 与 CMSIS
    ├── Qpc/                       QP/C 平台源码与 QV 移植
    ├── MDK-ARM/                   Keil 工程
    └── ThirdParty/                第三方库登记
```

## 快速开始

### 1. 使用 Keil 编译

1. 使用 Keil MDK-ARM 打开 `sensor-node-fw/MDK-ARM/sensor-node-fw.uvprojx`。
2. 确认安装 `Keil.STM32F1xx_DFP 2.4.1`，并选择 ARM Compiler 6。
3. 全量编译，目标是 `0 Error(s), 0 Warning(s)`。
4. 通过 SWD 下载生成的固件，或使用生成的 `.hex` 文件烧录。

CubeMX 重新生成代码后，在仓库根目录执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\apply_qp_to_keil.ps1
```

该脚本会恢复 Keil 工程中的文件分组、头文件路径、`QV_PRJ` 宏和 AC6 设置。

### 2. 无 Keil 时运行烟测

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw\App\tools\smoke_build.ps1
```

烟测用于检查源码编译、链接和基本镜像尺寸，不能替代 Keil ARM Compiler 6 的正式构建。

### 3. 串口联调

- 串口：USART1，PA9/PA10
- 参数：115200、8N1、无流控
- 复位后应看到启动横幅、AO 启动信息和仿真采样输出
- 发送 `AT` 加 CR/LF，预期返回 `ok`

业务串口与电源脚的完整分配见 `docs_sensor/03-资源分配.md`。

## 软件架构

依赖方向为：

```text
App 应用层 → Service 服务层 → Driver 器件驱动层 → BSP → HAL/CMSIS
```

QP/C 活动对象当前包括：

- `AO_Node`：调试串口 AT 控制台
- `AO_Sensor`：周期采样与数据处理
- `AO_Heartbeat`：运行状态心跳
- `AO_Ble`：VG6328 BLE 组件适配
- `AO_Modem`：MN316 4G 组件适配

业务代码应放在 `sensor-node-fw/App/`；CubeMX 生成代码只在 `Core/` 的 USER CODE 区域内修改。

## 文档入口

建议按以下顺序阅读：

1. `docs_sensor/00-索引.md`
2. `docs_sensor/01-项目介绍.md`
3. `docs_sensor/02-程序框架.md`
4. `sensor-node-fw/QP_INTEGRATION.md`
5. `sensor-node-fw/App/component/README.md`

所有有意义的代码、配置、脚本或文档改动，都需要同步记录到 `docs_sensor/08-改动日志.md`。

## Git 提示

本项目使用 Git 管理版本。Windows Git 可能提示 LF/CRLF 换行符转换，这是换行格式提示，不是编译错误。

常用命令：

```bash
git status
git add README.md
git add .
git commit -m "docs: add project README"
```

提交前请确认没有把不应入库的 Keil 构建产物、私密配置或大型外部资料加入暂存区。

## 当前限制

- 传感器仍为仿真驱动，真实 SCL3300 等器件尚未接入。
- 参数掉电保存尚未实现。
- Bootloader、OTA 和正式产品协议尚未实现。
- 最终的 ARM Compiler 6 构建尺寸需要在 Keil 环境中重新确认。
- 当前无硬件在环验证结论时，不应将 GCC 烟测当作上板验证。

## 许可证注意

QP/C 6.9.1 使用 GPLv3 / 商业双许可。闭源产品使用 QP/C 时，需要确认并满足相应的商业许可要求。具体来源与工程内改动见 `sensor-node-fw/Qpc/README.md`。
