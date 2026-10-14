---
title: 服务层规范
status: 进行中
owner: 设备端
updated: 2026-10-14
---

# 服务层（Service）

更新：2026-10-14。`svc_register.c/h` 新增可移植寄存器ASCII负载解析、CRC与逐地址结果编码，通过调用方注入read/write端口访问数据，不依赖App/QP/硬件。映射和串口/云端封装由App持有；请求先整帧校验再执行，最多读16项/写6对。怎么验证：`powershell -NoProfile -ExecutionPolicy Bypass -File sensor-node-fw/App/tools/register_test.ps1`，当前能力详见 `docs_sensor/06-协议格式.md`。

层次位置：**App 应用层 → Service 服务层 → Driver 器件驱动层 → BSP → HAL/CMSIS**

本层已经实现 `svc_at`（AT 解析）、`svc_log`（异步日志）、`svc_filter`（滤波）、`svc_frame`（CRC-16 组帧/解析）、
`svc_param`（**参数记录编解码 + 设备标识判据**，2026-09-28 订正：此前这里写着"参数存储等能力仍待开发"，
但 `svc_param` 早已落地并支撑 `AT24C04` 的 A/B 双槽记录；参数**存储介质与状态机**在 App 层的 `AO_Store`，
本层只负责编解码与校验）。

> [!note] 两条容易被抄错的约定（2026-09-28）
> - **AT 命令清单的唯一真相是 [[06-协议格式#命令一览]]**，本文件不复制命令表（复制必失真）。
> - **"这个值能不能当设备标识"** 由 `SVC_Param_CheckDeviceId()` 统一判定（非空 / 字符集 / 长度 / 出厂哨兵），
>   命令层写入与 4G 启动**共用同一个函数**；各写一份必然出现"一边收紧、一边放宽"。

## 职责

与硬件无关的中间处理：

- 协议打包 / 解析（上位机帧的组帧、校验、拆包）
- 参数存储（掉电保存的参数读写、默认值、版本迁移）
- 日志（分级、环形缓冲、异步输出）
- 数据滤波与换算（滑动平均、中值、标定曲线）

## 依赖规则

- 只允许调用 `App/driver/`、`App/bsp/` 以及 `ThirdParty/` 中的库
- **不允许** `#include "main.h"` / `stm32f1xx_hal.h` / `stm32f1xx.h`，不允许直接操作寄存器或外设
- 不允许调用 `App/app/` 的业务函数（依赖只能向下，不能向上）

## 命名与实现约定

| 项 | 约定 |
|---|---|
| 文件 | `svc_<模块>.c` / `svc_<模块>.h` |
| 函数 | `SVC_<模块>_<动作>()` |
| 类型 | `Svc<模块>`、`svc_<模块>_t` |
| 状态 | 优先写成无状态函数；确实需要状态时，做成纯数据结构或交给 App 层的 AO 持有 |

## 不该放在这里

- HAL / 寄存器 / 引脚操作 → 放 `App/bsp/`
- 器件时序与寄存器语义 → 放 `App/driver/`
- 状态机与业务规则 → 放 `App/app/`

## 落地检查清单

新增服务模块时：

1. 文件放在 `App/service/`，按上面的命名规则
2. Service 模块本身不是 AO；若业务需要新增 AO，按 `docs_sensor/05-任务说明.md#新增 AO 的步骤` 走，并更新 `App/app/app_signals.h` 与 `App/app/app_prio.h`
3. 运行 `App/tools/apply_qp_to_keil.ps1` 把新文件加入 Keil 分组（脚本会自动发现）
4. 同步 `docs_sensor/02-程序框架.md` 与相关文档的 `updated`
