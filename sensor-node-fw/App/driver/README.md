# 器件驱动层（Driver）

层次位置：App 应用层 → Service 服务层 → **Driver 器件驱动层** → BSP → HAL/CMSIS

本层已经实现 `drv_sensor.{h,c}` 统一接口，但当前只是仿真数据源；板上仍未接入真实传感器。

## 职责

把具体器件抽象成上层看得懂的语义接口，屏蔽总线与寄存器细节：

- 传感器：统一的 `read` / `write` 接口（原始值 + 量纲换算分离，换算放 Service）
- 存储器：EEPROM / Flash 参数区读写
- 显示屏、执行器等其他器件的统一访问接口

目标效果：上层只写 `DRV_Sensor_ReadTemp_cC()` 这类调用，不关心底层是仿真源、I2C 还是 SPI。

## 依赖规则

- 只允许调用 `App/bsp/` 里的接口（如后续新增的 `BSP_i2c_*` / `BSP_spi_*` / `BSP_uart_*`）
- 可以调用 `ThirdParty/` 里的算法或协议库
- **不允许**直接 `#include` HAL 头文件、不允许直接配置寄存器或引脚 —— 这些属于 BSP
- 不允许调用 `App/service/`、`App/app/`（依赖只能向下）

## 命名与实现约定

| 项 | 约定 |
|---|---|
| 文件 | `drv_<器件>.c` / `drv_<器件>.h`，例如 `drv_sht30.c` |
| 函数 | `DRV_<器件>_Init()`、`DRV_<器件>_Read()`、`DRV_<器件>_Write()` |
| 返回值 | `drv_status_t`：`DRV_OK` / `DRV_ERR_PARAM` / `DRV_ERR_BUS` / `DRV_ERR_TIMEOUT` / `DRV_ERR_NODEV` |
| 阻塞 | 允许短时间的总线等待；超过毫秒级的等待必须拆成"发起 → 事件驱动 → 继续"的异步接口，不能阻塞 AO |

## 落地检查清单

新增一个器件驱动时：

1. 先在 `App/bsp/` 里把所需总线封装成 `BSP_xxx()` 接口（禁止在驱动里直接碰 HAL）
2. 在 `App/driver/` 新建 `drv_<器件>.c/h`，实现 `Init/Read/Write` 与统一错误码
3. 用到的总线、引脚、从机地址同步登记到 `docs_sensor/03-资源分配.md`
4. 运行 `App/tools/apply_qp_to_keil.ps1` 把新文件加入 Keil 分组（脚本会自动发现）
5. 同步 `docs_sensor/02-程序框架.md` 与相关文档的 `updated`
