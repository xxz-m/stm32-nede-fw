---
title: BLE 组件裸机移植示例
status: 示例
owner: 待填
updated: 2026-09-14
---

# BLE 组件裸机移植示例

该目录展示最小裸机接入方式，不依赖 STM32 HAL、QP 或 RTOS。示例文件使用 `.example` 后缀，避免被宿主工程自动当作正式源码编译。

接收方按以下步骤使用：

1. 复制整个 `ble/` 目录到目标工程。
2. 用 `ble_component_config.example.h` 的内容替换 `ble_component_config.h`，按目标 RAM 调整容量。
3. 把 `ble_baremetal_example.c.example` 复制为自己的平台适配源码。
4. 实现示例顶部声明的 UART 启动、非阻塞发送、毫秒时钟和调试输出函数。
5. 在 UART RX ISR 中只调用 `BLE_Example_UartRxIsr(byte)` 存字节。
6. 上电初始化完成后调用 `BLE_Example_Init()`，主循环持续调用 `BLE_Example_Process()`。
7. 采样完成时调用 `BLE_Example_UpdateMeasurement()` 更新参数；组件认证完成后会按调用方策略上报。

如果底层 `BlePort` 已经做好，只想看产品业务怎样使用组件，请直接阅读 `ble_business_example.c.example`。它单独演示：

- 配置 SN、PIN、广播名称、Service UUID、设备名称、型号和批次；
- 定义业务参数、类型、单位、范围和可写属性；
- 接收小程序 `set sample_interval` 后更新产品调度周期；
- 传感器采样后更新当前值；
- 认证成功立即上报、周期上报和业务主动上报。

正式构建只加入：

```text
ble_component.c
ble_protocol.c
backend/ble_vg6328.c
你自己的 ble_port_xxx.c
```

不要把 `.example` 文件加入正式构建。
