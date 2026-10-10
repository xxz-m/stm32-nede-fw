/*============================================================================
* modem_component.h —— 芯片无关的 4G 云业务组件公共接口
* 层次定位：Component 层；对 App 只暴露本头文件与 modem_port.h，
*           具体模组的 AT 指令、URC 与错误映射全部封在 backend/ 内。
* 目标硬件：与具体模组无关（MN316 / NT26 由编译期后端选择）
*
* 约束（见 AGENTS.md 第 5.1 节与 App/component/README.md）：
*   - 只依赖标准 C（stdbool/stddef/stdint/string/stdio）与注入的 ModemPort
*   - 不使用动态内存、不定义全局可变状态、不包含 HAL/CMSIS/QP/RTOS 头
*   - 所有推进由 Modem_Component_Process() 完成，任何调用都不等待模组应答
*===========================================================================*/
#ifndef MODEM_COMPONENT_H
#define MODEM_COMPONENT_H

#include "modem_at.h"
#include "modem_component_config.h"
#include "modem_port.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*--------------------------------------------------------------- 逻辑主题 */
/*! 上层只认这些编号，不关心底层是主题字符串（MN316）还是通道+序号（NT26）。 */
typedef enum {
    MODEM_TOPIC_REPORT = 0,   /* 周期数据上报（上行）           */
    MODEM_TOPIC_READ,         /* 下行：读属性请求               */
    MODEM_TOPIC_READ_REPLY,   /* 上行：读属性应答               */
    MODEM_TOPIC_WRITE,        /* 下行：写配置请求               */
    MODEM_TOPIC_WRITE_REPLY,  /* 上行：写配置应答               */
    MODEM_TOPIC_INVOKE,       /* 下行：功能调用请求（暂未实现） */
    MODEM_TOPIC_STATE,        /* 下行：状态检查请求             */
    MODEM_TOPIC_STATE_REPLY,  /* 上行：状态检查应答             */
    MODEM_TOPIC_COMMAND,      /*!< 下行：AT 命令请求（平台规则 3.2） */
    MODEM_TOPIC_COMMAND_REPLY,/*!< 上行：AT 命令应答（平台规则 3.3 / 3.4） */
    MODEM_TOPIC_COUNT
} ModemTopic;

/*! 逻辑主题的收发方向；决定后端把它登记成订阅、发布还是两者。 */
typedef enum {
    MODEM_DIR_PUBLISH = 0,
    MODEM_DIR_SUBSCRIBE,
    MODEM_DIR_BOTH
} ModemDirection;

/*--------------------------------------------------------------- 配置与数据 */
/*! 连接与产品身份；由 App 层构造，绑定后其生命周期须覆盖组件。 */
typedef struct {
    char const *host;          /* 服务器域名或 IP                              */
    uint16_t port;             /* 服务器端口                                   */
    char const *product_id;    /* 产品标识，拼主题用（前导 '/' 按主题给，见 kTopicPrefix[]） */
    char const *device_id;     /* 设备标识，同时用作 MQTT client_id             */
    char const *username;      /* 认证用户名（日志中禁止打印）                  */
    char const *password;      /* 认证密码（日志中禁止打印）                    */
    uint16_t keep_alive_s;     /* MQTT 保活间隔，单位秒                         */
    uint32_t report_period_s;  /* 默认上报周期，单位秒                          */
} ModemConfig;

/*! 采样快照；由 App 层的 AO 填入（临时量），组件只读不改。
*  NOTE (2026-10-10)：湿度字段已删除——SCL3300-D01 没有湿度通道；
*  改为三轴倾角（0.01° 标度）。属性名见 modem_component.c 的 MODEM_PROP_ANGLE_*。 */
typedef struct {
    int32_t temperature_cC;    /* 片内温度，0.01 °C（传感器芯片自身温度）        */
    int32_t angle_x_cdeg;      /* X 轴倾角，0.01°                                */
    int32_t angle_y_cdeg;      /* Y 轴倾角，0.01°                                */
    int32_t angle_z_cdeg;      /* Z 轴倾角，0.01°                                */
    int32_t signal_quality;    /* 信号质量；入网阶段由后端从 CSQ 填入           */
    char tac[MODEM_COMPONENT_CELL_FIELD_MAX];      /* 跟踪区码，十六进制文本  */
    char cell_id[MODEM_COMPONENT_CELL_FIELD_MAX];  /* 小区标识，十六进制文本  */
    int32_t lon_e6;            /* 经度，1e-6 度（WGS-84）                        */
    int32_t lat_e6;            /* 纬度，1e-6 度（WGS-84）                        */
    bool location_valid;       /* true 表示本次上报带坐标                        */
} ModemMeasurements;

/*--------------------------------------------------------------- 状态与失败 */
typedef enum {
    MODEM_COMPONENT_IDLE = 0,  /* 未启动                                       */
    MODEM_COMPONENT_PROBING,   /* 探测模组是否在线                             */
    MODEM_COMPONENT_NETWORK,   /* SIM / 信号 / 注册 / 附着 / IP                */
    MODEM_COMPONENT_MQTT,      /* 配置并打开 MQTT、登记主题                    */
    MODEM_COMPONENT_READY,     /* 在线：周期上报 + 下行分发                    */
    MODEM_COMPONENT_FAILED     /* 连接失败，等待上层决定是否重连               */
} ModemComponentState;

typedef enum {
    MODEM_FAILURE_NONE = 0,
    MODEM_FAILURE_PORT_WRITE,  /* 串口写不进去（BSP 忙或链路错）               */
    MODEM_FAILURE_TIMEOUT,     /* 命令超时                                     */
    MODEM_FAILURE_RESPONSE,    /* 模组回了 ERROR/CME 或应答不符合预期          */
    MODEM_FAILURE_OVERFLOW,    /* 缓冲不够，命令未发出                         */
    MODEM_FAILURE_PLAN         /* 连接计划本身组装不出来                       */
} ModemFailure;

/*--------------------------------------------------------------- 主题槽 */
/*! 一个逻辑主题的登记记录；序号由核心统一分配（NT26 需要，MN316 忽略）。 */
typedef struct {
    char name[MODEM_COMPONENT_TOPIC_MAX];
    ModemDirection direction;
    bool registered;
    uint8_t subscribe_id;   /* 订阅序号，0 表示不订阅 */
    uint8_t publish_id;     /* 发布序号，0 表示不发布 */
    uint8_t publish_qos;    /* 发布 QoS（NT26 在登记主题时固定） */
} ModemTopicSlot;

/*--------------------------------------------------------------- 运行期操作 */
/*! READY 态下正在执行的一次性动作；同一时刻最多一个。 */
typedef enum {
    MODEM_RUNTIME_NONE = 0,
    MODEM_RUNTIME_REPLY,    /* 发布一条命令应答 */
    MODEM_RUNTIME_REPORT,   /* 发布一条周期数据 */
    MODEM_RUNTIME_CELL,     /* 查一次小区信息   */
    MODEM_RUNTIME_LOCATION  /* 查一次基站定位（后端不支持则不发命令） */
} ModemRuntimeOp;

/*--------------------------------------------------------------- 组件实例 */
/*! 全部缓冲都是实例内的静态数组：不使用动态内存，也不跨层传递缓冲所有权。 */
typedef struct {
    ModemPort const *port;
    ModemConfig const *config;
    ModemMeasurements data;

    ModemComponentState state;
    ModemFailure failure;
    uint8_t failure_step;

    /* --- AT 引擎与连接计划推进 --- */
    ModemAt at;
    uint8_t plan_step;
    uint8_t attempt;
    uint32_t step_started_ms;
    uint32_t step_deadline_ms;
    uint32_t next_attempt_ms;
    bool step_delay_active;
    uint32_t step_delay_until_ms;
    bool session_reused;
    /** 模组里存的 host/port/clientId 是否与**我们当前的身份**一致。
     * @note 由 `MODEM_OP_MQTT_CFG_QUERY`（`AT+MQTTCFG?`）写入，`MODEM_OP_MQTT_STATE`
     *   用它决定"状态 5 时能不能复用会话"：**必须一致才复用**，否则走完整重建，
     *   否则 clientId 会停留在旧值（设备身份 `SN` 改过之后的具体后果）。
     *   初值与失败值都是 `false`——"读不到就不复用"是安全方向。 */
    bool remote_cfg_matches;
    /** 公网可达性探测（`MODEM_OP_NET_PROBE_*`）里 `AT+NSOCR` 返回的 socket 编号。
     * @note 为什么放在核心而不是后端：后端被要求「不持有静态状态」，所有跨步骤的
     *   状态都由 ModemComponent 持有（见 modem_backend.h 的文件头约定）。探测是一组
     *   四步的序列，后三步都要用这个编号，所以只能放这里。不探测时保持 0 且无意义。 */
    uint8_t probe_socket;
    bool recover_done;

    /* --- 收发缓冲 --- */
    char command[MODEM_COMPONENT_COMMAND_MAX];
    char response[MODEM_COMPONENT_RESPONSE_MAX];
    size_t response_len;
    char line[MODEM_COMPONENT_LINE_MAX];
    size_t line_len;
    bool line_overflow;
    char urc[MODEM_COMPONENT_URC_MAX];
    size_t urc_len;
    bool urc_ready;
    char payload[MODEM_COMPONENT_PAYLOAD_MAX];

    /* --- 主题与上行队列 --- */
    ModemTopicSlot topic[MODEM_TOPIC_COUNT];
    uint8_t topic_cursor;       /* TOPIC 步正在下发的主题下标    */
    uint8_t next_subscribe_id;  /* 下一个可分配的订阅序号（从 1） */
    uint8_t next_publish_id;    /* 下一个可分配的发布序号（从 1） */
    char reply_payload[MODEM_COMPONENT_PAYLOAD_MAX];
    ModemTopic reply_topic;
    uint8_t reply_qos;
    bool reply_pending;

    /* --- 云端 AT 命令下行（平台规则 3.2 的 command 主题） ---
     * NOTE (为什么由组件拆包、由 App 执行)：本组件不许访问参数存储或 QP，
     *   所以它只把 `id` 与 `command` 两个字段原样取出来交给 App；执行与结果
     *   由 App 层用统一命令分发器完成，再经 CompleteCommand() 回到这里发布。 */
    char downlink_id[MODEM_COMPONENT_DOWNLINK_ID_MAX];
    char downlink_command[MODEM_COMPONENT_DOWNLINK_COMMAND_MAX];
    bool downlink_pending;   /*!< true = 有一条待 App 执行的命令 */

    /* --- 运行期状态 --- */
    ModemRuntimeOp runtime_op;
    bool report_requested;
    bool cell_requested;
    bool location_requested;
    /* NOTE (2026-09-29 删掉 `sequence`)：它原来只服务于报文里的 `"seq":N`，而平台定的
     * 报文形状是 `{"deviceId":…,"properties":{…}}`（没有 seq 这个键）。留一个只增不读的
     * 计数没有意义，要查丢包应当走平台已定义的属性标识符（见 `modem_component.c` 的
     * `MODEM_PROP_*` 注释），而不是往报文里塞自研字段。 */
    uint32_t report_period_ms;
    uint32_t cell_period_ms;
    /* --- 在线期上行的容错（2026-09-20 新增） --- */
    uint8_t uplink_retry;         /*!< 本轮连续失败的上行次数；成功即清零     */
    uint32_t uplink_retry_at_ms;  /*!< 下一次允许重发上行的时刻（退避节流）   */
    /*! 后端 recover 的阶段计数：由后端自己解释（MN316 用 0/1/2 表示
     *  「未补救 → 已关射频 → 已开射频并强制选网」）；每次 Start 时清零。 */
    uint8_t recover_phase;

    /* --- 产品侧可写参数 --- */
    uint32_t sample_interval_min;
    bool report_enabled;
} ModemComponent;

/*--------------------------------------------------------------- 公共接口 */
/** @brief 绑定端口与配置并清零状态；不碰串口，可随时调用。 */
void Modem_Component_Init(ModemComponent *, ModemPort const *, ModemConfig const *);

/** @brief 启动一次连接：生成主题、复位计划并发出第一步命令。 */
void Modem_Component_Start(ModemComponent *);

/** @brief 推进状态机；由上层周期调用（建议 10~50 ms 一次）。
* @note  本函数不做任何等待：读走串口已有字节、喂行、判期限，然后立刻返回。 */
void Modem_Component_Process(ModemComponent *);

/** @brief 请求尽快上报一次数据（下个 Process 周期发出）。 */
void Modem_Component_RequestReport(ModemComponent *);

/** @brief 请求查一次小区信息（下个 Process 周期发出）。 */
void Modem_Component_RequestCellInfo(ModemComponent *);

/** @brief 请求查一次基站定位（下个 Process 周期发出）。
* @note 后端未实现（`ModemBackend.location_build` 为空）时本请求被静默丢弃；
*       定位失败只影响坐标，不影响连接状态与上一次的有效坐标。 */
void Modem_Component_RequestLocation(ModemComponent *);

/** @brief 是否已连上云端（可上报、可下发）。 */
bool Modem_Component_IsReady(ModemComponent const *);

/*------------------------------------------------ 云端 AT 命令下行（平台规则） */
/** @brief 取走一条待执行的云端 AT 命令（`command` 主题）。
 *
 * @param me              组件实例
 * @param id              出参：平台关联 ID（必须原样回显），可为 NULL
 * @param idCapacity      id 的容量
 * @param command         出参：AT 命令文本（**不含** CR/LF），可为 NULL
 * @param commandCapacity command 的容量
 * @return true = 取到一条命令并已清空待办标志；false = 当前没有待办命令
 *
 * @note 由 App 层在每次 `Modem_Component_Process()` 之后轮询调用。组件不会自己执行
 *       命令（它不认识参数存储，也不允许依赖 QP）；执行完必须调用
 *       `Modem_Component_CompleteCommand()` 回一条应答，否则平台侧会一直等。
 * @note 缓冲不够时命令被丢弃并返回 false（平台允许 256 字节，本设备行上限见
 *       `MODEM_COMPONENT_DOWNLINK_COMMAND_MAX`），此时 App 应回失败应答。
 */
bool Modem_Component_TakeCommand(ModemComponent *me,
                                 char *id, size_t idCapacity,
                                 char *command, size_t commandCapacity);

/** @brief 发布一条 `command/reply`（平台规则 3.3 / 3.4）。
 *
 * @param me  组件实例
 * @param id  平台关联 ID，原样回显
 * @param ok  true = 成功回复（`ok:true` + `data`）；false = 失败回复（`ok:false` + `error` + `data`）
 * @param raw 完整原始 AT 回复文本（含行尾；成功必须以 `OK\r\n` 结尾，失败必须以 `ERROR\r\n` 结尾）
 *
 * @note JSON 拼装（含转义）在组件内完成：App 只提供语义，不碰报文格式。
 *       报文必须是单层 JSON，字段固定为 `id` / `ok` / `error` / `data`。
 */
void Modem_Component_CompleteCommand(ModemComponent *me, char const *id, bool ok,
                                     char const *raw);

#endif /* MODEM_COMPONENT_H */
