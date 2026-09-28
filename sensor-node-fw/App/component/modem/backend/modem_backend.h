/*============================================================================
* modem_backend.h —— 4G 芯片后端内部接口
* 层次定位：Component 层内部头；只有 modem_component.c 与 backend 目录下的后端实现包含它，
*           对 App 不可见（App 只包含 modem_component.h）。
*
* 后端职责边界：把「连上云端」拆成一张线性的步骤表，并为每一步提供
*   - 命令文本与期望应答标记
*   - 应答解析（把 +CEREG / +CSQ / 通道状态等折进组件字段）
*   - 主动通知（URC）的识别与解析
*   - 运行期的发布命令、小区查询命令与基站定位查询命令（后两者可选）
* 业务协议（主题组织、上报报文、下行命令分发）一律留在核心，后端不得引用。
*
* NOTE (无全局状态)：后端全部实现都是纯函数，状态放在 ModemComponent 里
*   （主题槽、序号游标、补救标志），因此同一份后端可以被多个实例共用。
*===========================================================================*/
#ifndef MODEM_BACKEND_H
#define MODEM_BACKEND_H

#include "modem_component.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*! 连接计划里的一步属于哪个语义动作；核心用它推导组件状态与日志文案。 */
typedef enum {
    MODEM_OP_NONE = 0,
    /* --- 探测段 --- */
    MODEM_OP_PROBE,            /* AT 探活                       */
    /* --- 入网段 --- */
    MODEM_OP_ECHO_OFF,         /* 关回显                        */
    MODEM_OP_CMEE,             /* 打开详细错误码（可选）        */
    MODEM_OP_RADIO,            /* 查射频状态（可选）            */
    MODEM_OP_SIM,              /* 等 SIM / IMSI                 */
    MODEM_OP_IDENTITY,         /* 读 IMEI（诊断用）             */
    MODEM_OP_SIGNAL,           /* 读 CSQ                        */
    MODEM_OP_SIGNAL_EXT,       /* 读 CESQ（可选，诊断用）       */
    MODEM_OP_OPERATOR,         /* 读 COPS（诊断用）             */
    MODEM_OP_REG_MODE,         /* 打开位置信息结果码            */
    MODEM_OP_REGISTRATION,     /* 等网络注册（1 或 5）          */
    MODEM_OP_ATTACHED,         /* 等分组域附着                  */
    MODEM_OP_IP_ADDRESS,       /* 取 IP 地址                    */
    /* --- MQTT 段 --- */
    MODEM_OP_CHANNEL_CLOSE,    /* 先关通道，再改参数            */
    MODEM_OP_MQTT_DISC,        /* 请求断开 MQTT 连接（可选）    */
    MODEM_OP_TASK,             /* 把通道定义成 MQTT             */
    MODEM_OP_PLATFORM,         /* 选择 MQTT 平台                */
    MODEM_OP_SERVER,           /* 服务器地址与端口              */
    MODEM_OP_AUTH,             /* client_id / 用户名 / 密码     */
    MODEM_OP_MSGHEAD,          /* 打开消息头（下行带主题）      */
    MODEM_OP_LBS_MODE,         /* 定位方式（可选）              */
    MODEM_OP_MQTT_TIMEOUT,     /* 模组 MQTT 超时参数            */
    MODEM_OP_MQTT_DELETE,      /* 清理残留客户端                */
    MODEM_OP_MQTT_CONFIG,      /* 客户端参数                    */
    MODEM_OP_MQTT_CFG_QUERY,   /* 读回客户端参数（复用会话前比对，2026-09-28） */
    MODEM_OP_MQTT_OPEN,         /* 发起连接                      */
    MODEM_OP_MQTT_STATE,       /* 等连接状态就绪                */
    MODEM_OP_TOPIC_SUB,        /* 逐条订阅下行主题              */
    MODEM_OP_TOPIC_PUB,        /* 逐条登记上行主题              */
    MODEM_OP_CHANNEL_OPEN,     /* 打开通道                      */
    MODEM_OP_CHANNEL_STATE,    /* 等通道在线                    */
    MODEM_OP_ROUTE_DOWN,       /* 下行透传路由（网络通道 → 串口） */
    MODEM_OP_PDP,              /* 读 PDP 上下文 / APN（只读诊断，2026-09-27 追加） */
    /* --- 公网可达性探测（只读诊断，2026-09-27 追加）--------------------------------
     * 动机：电信卡与移动卡在同一块板、同一份固件上表现截然不同（移动稳定连上、
     *   电信一律倒在 AT+MQTTOPEN）。而 `+MQTTOPEN:FAIL` 的手册定义只是「连接服务器
     *   失败」，区分不了「这张卡根本出不了公网（定向/策略拦截）」与「只是连不上
     *   我们这台服务器」。本组步骤用模组自带的裸 TCP 能力去连一个**与业务无关**的
     *   公网地址（默认阿里 DNS 223.5.5.5:53），把这两件事分开。
     * 判据（MN316 AT 命令用户手册 §5，第 78~85 页）：
     *   - `AT+NSOCO` 的 OK **只表示「开始连接」**，不是连上了；
     *   - 真正的证据是 `AT+NSOSD`（**发送 TCP 数据**，§5.6 第 84 页）之后收到
     *     `+NSOSTR`——批注为「发送成功并**收到对方 ACK**」。所以本组成功标记取
     *     `+NSOSTR`，不是 OK。
     *   - ⚠ **命令名别写错**：是 `AT+NSOSD`，**不是** `AT+NSOD`。《MN316_通信流程示例》
     *     第 22 页的示例里把它写成了 `AT+NSOD`，那是**笔误**——模组没有这条命令，
     *     发错会立刻回 `+CME ERROR:4`（不支持的操作），看起来非常像「连不上」。
     *     2026-09-27 上板已踩过这个坑，见 [[08-改动日志]] 的 `#43`。
     * 开关：`APP_CFG_MODEM_NET_PROBE_ENABLE = 0` 时后端 build() 直接返回 false，
     *   四个步骤被 startPlanStep() 原地跳过，**不发任何 AT、不占任何时间**。 */
    MODEM_OP_NET_PROBE_OPEN,     /* 建 socket：AT+NSOCR  */
    MODEM_OP_NET_PROBE_CONNECT,  /* 连目标：  AT+NSOCO  */
    MODEM_OP_NET_PROBE_SEND,     /* 发数据：  AT+NSOSD（等 +NSOSTR 作为 ACK 证据） */
    MODEM_OP_NET_PROBE_CLOSE,    /* 关 socket：AT+NSOCL（必须关，模组只支持 2 个） */
    /* NOTE (新增 op 的位置)：必须在 MODEM_OP_COUNT 之前「追加」，
     *   不能插到中间——已有后端的计划表按下标取值，插队会静默改变它们的语义。
     *   本枚举只增不改；modem_component.c 的 stateOfOp() 按区间判断状态，
     *   追加在 IP_ADDRESS 之后的 op 自然归入 MODEM_COMPONENT_MQTT，无需改核心。
     *   （`MODEM_OP_PDP` 与上面这组探测 op 例外：它们语义上属于「入网」阶段，
     *   stateOfOp() 里单独放行到 MODEM_COMPONENT_NETWORK，只是为了让状态显示准确。） */
    MODEM_OP_COUNT
} ModemOp;

/*! 一步的执行方式。 */
typedef enum {
    MODEM_STEP_ONCE = 0,   /* 发一次；失败即整条计划失败                 */
    MODEM_STEP_OPTIONAL,   /* 发一次；失败或超时也继续（诊断/兼容类命令） */
    MODEM_STEP_REPEAT,     /* 失败后按 retry_ms 重发，直到 budget_ms 用尽 */
    MODEM_STEP_TOPIC,      /* 按 topic_cursor 逐条下发主题                */
    /*! 像 REPEAT 一样按 retry_ms 重发、按 budget_ms 计时，但**预算用尽后继续**
    *   而不是让整条计划失败。
    * @note 为什么需要它（2026-09-27，MN316 手册）：`+CME ERROR:8002` 的手册定义是
    *   「操作不允许，通常为状态机不符合」——它是**可等待的瞬时状态**，不是终态。
    *   `AT+MQTTDEL` 在模组还没释放完资源时就会回它，等 1 秒再发往往就正常了。
    *   用 REPEAT 会把这种可恢复情形升级成「整条连接失败」；用 OPTIONAL 又只发一次、
    *   不给模组留恢复时间。两者的交集就是本模式。 */
    MODEM_STEP_OPTIONAL_REPEAT
} ModemStepMode;

/*! 计划中的一步。 */
typedef struct {
    ModemOp op;
    uint8_t mode;
    uint32_t timeout_ms;  /* 单次命令的应答超时                            */
    uint32_t retry_ms;    /* REPEAT 的重发间隔，也是串口忙时的重试间隔    */
    uint32_t budget_ms;   /* REPEAT 的总预算（0 表示不限，靠 attempts）   */
    uint8_t attempts;     /* 最少尝试次数（>= 1）                          */
    /** 可容忍的错误串；NULL/"" 表示不容忍。
    * @note 有些命令的「失败」其实正是期望状态。声明后，核心在本步收到
    *   MODEM_AT_ERROR、**且应答文本里确实出现该串**时按成功处理。
    * @note 追加在结构体末尾：已有后端的计划表按位置初始化，会自动把本字段补成 NULL，
    *   因此加这一项不会改变 ML307C / NT26 的任何行为。
    * @note **慎用**（2026-09-27）：它一旦命中就当场判成功，**跳过本步的所有重试**。
    *   对于「等一等就正常」的瞬时状态（典型是 `+CME ERROR:8002`）应该改用
    *   `MODEM_STEP_OPTIONAL_REPEAT` 先重试，而不是用本字段一步放行。 */
    char const *tolerate;
    /** 当前步成功后、下一步发送前的非阻塞稳定等待（毫秒）。 */
    uint32_t post_success_delay_ms;
} ModemStep;

/*! 后端实现表；每个固件只编入一份。 */
typedef struct {
    char const *name;
    ModemStep const *plan;
    uint8_t plan_count;

    /** 组装连接计划第 step 步的命令与期望标记。
    * @return false 表示当前配置下这一步应被跳过（例如没有可下发的主题） */
    bool (*build)(ModemComponent *me, uint8_t step, char *expected, size_t expected_size);
    /** 应答收到期望标记后的解析；返回 false 表示这一步其实没通过。 */
    bool (*apply)(ModemComponent *me, uint8_t step);
    /** REPEAT 步预算耗尽时的补救（如重启模组）；返回 true 表示已补救并刷新期限。
    * @note 核心会连续调用它：只要仍返回 true，该步就继续被重试；返回 false 才判失败。
    *   因此**分阶段**的补救（先关射频、等一会儿再开射频）可以用 `me->recover_phase`
    *   记阶段实现——该字段由核心在每次 `Modem_Component_Start()` 时清零，语义由后端
    *   自己解释（见 `mn316Recover()`）。 */
    bool (*recover)(ModemComponent *me, uint8_t step);

    bool (*urc_starts)(char const *text, size_t length);
    bool (*urc_complete)(char const *text, size_t length);
    bool (*urc_extract)(ModemComponent *me, char const *text, size_t length,
                        ModemTopic *topic, char *payload, size_t payload_size);

    bool (*publish_build)(ModemComponent *me, ModemTopic topic, char const *payload,
                          char *expected, size_t expected_size);
    bool (*cell_build)(ModemComponent *me, char *expected, size_t expected_size);
    bool (*cell_apply)(ModemComponent *me);

    /*! 基站定位查询（可选）。两个都为空表示本后端不做定位，核心会直接清掉请求、
    *   不发任何命令（NT26 / MN316 当前如此）。
    * NOTE (尽力而为)：定位失败是常态而不是异常——室内或基站稀疏时模组会回错误码
    *   （ML307C 为 +CME ERROR: 130）。因此核心在失败时只清请求标志，既不改变连接
    *   状态，也不清掉上一次取到的有效坐标；后端只需在解析成功时提交新的经纬度。 */
    bool (*location_build)(ModemComponent *me, char *expected, size_t expected_size);
    bool (*location_apply)(ModemComponent *me);
} ModemBackend;

/** @brief 取当前编译进固件的后端实现表（由 modem_component.c 按宏选择）。 */
ModemBackend const *Modem_Backend_Get(void);

/** @brief 后端实现表实例；未选中的那份用 #if 排除，不产生代码。 */
#if (MODEM_COMPONENT_BACKEND == MODEM_BACKEND_MN316)
extern ModemBackend const Modem_Mn316_Backend;
#elif (MODEM_COMPONENT_BACKEND == MODEM_BACKEND_NT26)
extern ModemBackend const Modem_Nt26_Backend;
#elif (MODEM_COMPONENT_BACKEND == MODEM_BACKEND_ML307C)
extern ModemBackend const Modem_Ml307c_Backend;
#else
#error "APP_CFG_MODEM_BACKEND 取值无效：只能是 APP_CFG_MODEM_BACKEND_MN316、APP_CFG_MODEM_BACKEND_NT26 或 APP_CFG_MODEM_BACKEND_ML307C"
#endif

#endif /* MODEM_BACKEND_H */
