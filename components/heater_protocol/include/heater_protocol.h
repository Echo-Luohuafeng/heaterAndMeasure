#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HEATER_CHANNEL_COUNT 4
#define HEATER_MAX_DUTY 95
#define HEATER_COMMAND_MAX_LENGTH 64
#define HEATER_LINE_MAX_LENGTH 256
#define HEATER_PERIOD_US 20000U
#define HEATER_ADC_GAIN 4U

// 定义设定、停止、续约和显式开始四种控制操作；SET/PING 不启动空闲运行。
typedef enum {
    HEATER_COMMAND_SET,
    HEATER_COMMAND_STOP,
    HEATER_COMMAND_PING,
    HEATER_COMMAND_START,
    HEATER_COMMAND_RESISTANCE_START,
} heater_command_kind_t;

// 保存解析后的控制命令；仅 SET/START 使用 duty，四路值均为 0～95。
typedef struct {
    heater_command_kind_t kind;
    uint8_t duty[HEATER_CHANNEL_COUNT];
} heater_command_t;

// 定义可按位组合的周期故障标志，供通道许可判定及上位机诊断使用。
enum {
    HEATER_FAULT_TIMING = 1U << 0,        // 阶段中断或控制任务错过时序。
    HEATER_FAULT_ADC_TIMEOUT = 1U << 1,   // 未收到或未读完全部 ADC 通道。
    HEATER_FAULT_SPI = 1U << 2,           // ADC 命令或数据传输失败。
    HEATER_FAULT_SAMPLE = 1U << 3,        // 原始码饱和、非正值或电阻越界。
    HEATER_FAULT_HOST_TIMEOUT = 1U << 4,  // 控制租约到期，等待显式 START/STOP。
    HEATER_FAULT_STOPPED = 1U << 5,       // 当前周期收到 STOP。
    HEATER_FAULT_CONTROLLER = 1U << 7, // 算法超时/无效输出，取消运行。
    HEATER_FAULT_PWM = 1U << 6,           // PWM 操作失败，加热锁定至重启。
};

// schema 2 实验字段；pwm_ticks 为实际锁存计数，每计数 0.5 个百分点。
typedef struct {
    uint32_t run_id, config_id, source_sequence, computation_us;
    uint8_t mode, algorithm, limited_mask, blocked_mask;
    uint8_t pwm_ticks[4];
    float target[4], requested[4];
} heater_experiment_t;

// 保存一个完整周期的测量快照，按值跨任务传递；时间单位为 µs、电阻单位为 Ω。
// valid_mask 位 0～3 标识四通道，无效电阻为 NAN；duty 为周期起点生效值。
// 故障可能提前终止加热，诊断字段报告时序迟到数、队列丢帧数与栈余量（字节）。
typedef struct {
    uint32_t sequence;
    uint64_t cycle_start_us;
    uint8_t duty[HEATER_CHANNEL_COUNT];
    float resistance_ohm[HEATER_CHANNEL_COUNT];
    uint8_t valid_mask;
    uint32_t faults;
    uint32_t deadline_misses;
    uint32_t telemetry_drops;
    uint32_t control_stack_free_bytes;
    heater_experiment_t experiment;
} heater_telemetry_t;

// 区分数据帧与兼容文本回复；当前历史缓存只消费 DATA。
typedef enum {
    HEATER_MESSAGE_DATA,
    HEATER_MESSAGE_REPLY,
} heater_message_kind_t;

// 定义队列中按值复制的消息，kind 决定有效联合成员。
// DATA 的 run_id 必须对应当前显式运行，避免 STOP 后或新运行混入旧样本。
typedef struct {
    heater_message_kind_t kind;
    uint32_t run_id;
    union {
        heater_telemetry_t data;
        char reply[HEATER_COMMAND_MAX_LENGTH];
    };
} heater_message_t;

// 解析长度为 `length` 的命令到非空的 `command`，`line` 无需以 NUL 结尾。
// 接受 START/SET 加四路 0～95 整数，以及无参数的 STOP/PING；不接受换行符。
// 指针为空、长度或语法非法时返回 false，且不改写 `command`。
bool heater_command_parse(const char *line, size_t length, heater_command_t *command);
// 将非空的三字节大端补码 `bytes` 符号扩展为 int32_t。
// 返回范围为 -8388608～8388607 的 ADC 原始码。
int32_t heater_decode_adc24(const uint8_t bytes[3]);
// 将 ADC 原始码 `code` 按增益 4 换算为电阻，返回单位为 Ω。
// `reference_mohm` 为参考电阻，`offset_mohm` 为校准偏移，单位均为 mΩ。
// 本函数只进行换算；有效范围与饱和检查由控制模块完成。
float heater_resistance_ohm(int32_t code, uint32_t reference_mohm, int32_t offset_mohm);
// 将非空的 `data` 格式化为兼容旧 USB 协议的 DATA 文本行。
// `output` 必须可写，`capacity` 包含结尾 NUL；返回不含 NUL 的长度。
// 格式化失败或空间不足返回 -1，缓冲区可能已写入截断文本。
int heater_telemetry_format(const heater_telemetry_t *data, char *output, size_t capacity);
