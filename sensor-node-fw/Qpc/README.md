# QP/C 6.9.1（随工程引入的源码）

本目录是为了构建 **QV 协作式内核** 而引入的 QP/C 6.9.1 源码：

- `platform/inc`、`platform/src` —— 与平台无关的 QP/C 内核与基础设施（QEP / QF / QV）
- `port/qv/inc`、`port/qv/src` —— QV 内核的 ARM Cortex-M 移植层，来源为 QP/C 6.9.1 官方发行版中的 “ARM-KEIL（ARM Compiler 5）” 端口

## 许可证

QP/C 由 Quantum Leaps, LLC 双许可授权：

- 开源：**GNU GPLv3**（或更新版本）；GPL 要求衍生作品也以 GPL 发布；**或**
- 商业许可：面向需要保留闭源的产品。

许可说明见 <https://www.state-machine.com/licensing>，上游仓库为 <https://github.com/QuantumLeaps/qpc>。
**闭源产品必须购买商业许可**（或者不使用 QP/C）。

## 本仓库做的改动（ARM Compiler 6 / armclang 兼容）

6.9.1 的 ARM-Keil 端口是针对 ARM Compiler 5 写的，直接用 ARM Compiler 6（armclang）编译会失败。为使其在 **ARM Compiler 5、ARM Compiler 6、GCC** 下都能编译，做了以下最小改动，每处都在源码里带中文 `NOTE` 注释：

| 文件 | 改动 |
|---|---|
| `port/qv/inc/qep_port.h` | `Q_NORETURN`：AC5 用 `__declspec(noreturn)`，armclang/GCC 用 `__attribute__((noreturn))` |
| `port/qv/inc/qf_port.h` | 顶部增加 `#include "cmsis_compiler.h"`；`QF_get_BASEPRI()` / `QF_set_BASEPRI()` 改用 CMSIS 的 `__get_BASEPRI()` / `__set_BASEPRI()`，不再使用 AC5 的具名寄存器变量；`QF_CRIT_EXIT_NOP()` 在 armclang/GCC 下使用 `__asm volatile` |
| `port/qv/inc/qf_port.h` | `QF_LOG2()` 在 armclang/GCC 下使用 `__builtin_clz()`（AC5 用 `__clz()`） |
| `port/qv/inc/qv_port.h` | `QV_CPU_SLEEP()` 改用 CMSIS 的 `__WFI()`（AC5 的 `__wfi()` 在 armclang 下不可用）；`QV_ARM_ERRATUM_838869()` 在 armclang/GCC 下使用 `__asm volatile` |

除上述改动外，**上游代码与原注释保持原样**，便于将来整体升级替换。

## 没有加入编译的部分

本工程只使用 QV 内核（`platform/src/qv.c` + `port/qv/src/qv_port.c`）。
`platform/src` 下的 `qk.c`、`qxk*.c` 仅作留存：它们是 **ARM Compiler 5 的内联汇编写法**（`__asm void ... Handler(void)`），在 ARM Compiler 6 下无法编译。若要切换抢占式内核，请改用官方 QP/C 7.x / 8.x 提供的 armclang 端口。
