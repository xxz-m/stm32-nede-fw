# 服务层（Service）

层次位置：**App 应用层 → Service 服务层 → Driver 器件驱动层 → BSP → HAL/CMSIS**

本层已经实现 `svc_at`、`svc_log`、`svc_filter`、`svc_frame`；参数存储等能力仍待开发。

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
