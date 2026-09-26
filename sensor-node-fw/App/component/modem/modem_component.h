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
    char const *product_id;    /* 产品标识，用于拼主题 /{productId}/{deviceId}/.. */
    char const *device_id;     /* 设备标识，同时用作 MQTT client_id             */
    char const *username;      /* 认证用户名（日志中禁止打印）                  */
    char const *password;      /* 认证密码（日志中禁止打印）                    */
    uint16_t keep_alive_s;     /* MQTT 保活间隔，单位秒                         */
    uint32_t report_period_s;  /* 默认上报周期，单位秒                          */
} ModemConfig;

/*! 采样快照；由 App 层的 AO 填入（临时量），组件只读不改。 */
typedef struct {
    int32_t temperature_cC;    /* 温度，0.01 °C                                 */
    uint16_t humidity_permil;  /* 相对湿度，0.1 %RH                             */
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

    /* --- 运行期状态 --- */
    ModemRuntimeOp runtime_op;
    bool report_requested;
    bool cell_requested;
    bool location_requested;
    uint32_t sequence;
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

#endif /* MODEM_COMPONENT_H */
