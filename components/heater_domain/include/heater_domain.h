// 四路加热测量的业务值类型与纯函数，不依赖 ESP-IDF。
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "controller_types.h"
#ifdef __cplusplus
extern "C" {
#endif
#define HEATER_CHANNEL_COUNT 4
#define HEATER_MAX_DUTY 95
#define HEATER_COMMAND_MAX_LENGTH 64
#define HEATER_PERIOD_US 20000U
#define HEATER_ADC_GAIN 4U
#define HEATER_CONFIG_CAPACITY 64U
// 设备生命周期、实验状态与控制模式分别表达，避免状态组合含义不清。
typedef enum {
  HEATER_DEVICE_INITIALIZING,
  HEATER_DEVICE_READY,
  HEATER_DEVICE_RECOVERY,
  HEATER_DEVICE_PAUSING,
  HEATER_DEVICE_PAUSED
} heater_device_state_t;
typedef enum { HEATER_RUN_IDLE, HEATER_RUN_RUNNING } heater_run_status_t;
typedef enum { HEATER_MODE_MANUAL, HEATER_MODE_RESISTANCE } heater_mode_t;
typedef enum {
  HEATER_CONFIG_EMPTY,
  HEATER_CONFIG_PREPARING,
  HEATER_CONFIG_READY,
  HEATER_CONFIG_FAILED
} heater_config_state_t;
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
  uint32_t config_id;  // RSTART 可绑定已准备的配置；零兼容旧命令。
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
  HEATER_FAULT_CONTROLLER = 1U << 7,    // 算法超时/无效输出，取消运行。
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

// 采样信封按值传递；身份与取消代次用于拒绝旧队列消息。
typedef struct {
  uint32_t run_id;
  uint32_t generation;
  heater_telemetry_t data;
} heater_message_t;

// 每份配置不可变；仍有历史引用时，记录器禁止回收它。
typedef struct {
  uint32_t config_id;
  heater_mode_t mode;
  controller_config_t config;
  uint8_t manual_ticks[HEATER_CHANNEL_COUNT];
  uint32_t reference_mohm[HEATER_CHANNEL_COUNT];
  int32_t offset_mohm[HEATER_CHANNEL_COUNT];
  char algorithm_version[16];
} heater_config_record_t;
// 运行、时序和算法状态快照都是纯值类型，接口层无需包含实时实现。
typedef struct {
  bool running;
  uint32_t id;
  uint32_t generation;
} heater_run_state_t;
typedef struct {
  uint32_t adc_start_max_offset_us;
  uint32_t adc_start_misses;
  uint32_t adc_read_misses;
} heater_timing_stats_t;
typedef struct {
  bool resistance_mode;
  bool ready;
  heater_config_state_t state;
  uint32_t config_id;
  controller_config_t config;
  uint32_t worker_stack_free_bytes;
  uint32_t controller_faults;
  uint32_t lease_refreshes, lease_remaining_ms;
} heater_controller_status_t;

// ADC 补码解码和比例电阻换算，单位分别为原始码及 Ω。
int32_t heater_decode_adc24(const uint8_t bytes[3]);
float heater_resistance_ohm(int32_t code, uint32_t reference_mohm,
                            int32_t offset_mohm);
// 纯规则供实时采样与主机测试共同使用。
bool heater_sample_valid(int32_t code, float resistance, float minimum,
                         float maximum);
bool heater_duties_valid(const uint8_t duty[4], unsigned limit);
#ifdef __cplusplus
}
#endif
