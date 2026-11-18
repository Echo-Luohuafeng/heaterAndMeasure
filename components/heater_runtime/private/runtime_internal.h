// 实时内核内部契约；外部业务禁止包含此头文件。
#pragma once
#include <string.h>

#include "controller_worker.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heater_board.h"
#include "heater_controller.h"
#include "heater_runtime.h"
#include "sdkconfig.h"
#define CONTROL_TASK_STACK_BYTES 6144
#define CONTROL_TASK_PRIORITY 20
#define ADC_START_US 16800U
// 系统调度与 PSRAM 缓存波动使四路 START 即使在 240 MHz 下也可能超过 200 µs。
// 允许到 17.10 ms 完成，但测量关断仍固定为周期起点后的 19.05 ms。
#define ADC_START_DEADLINE_US (ADC_START_US + 300U)
#define MEASURE_OFF_US 19050U
#define ADC_READ_GUARD_US 100U
#define ADC_EARLIEST_READY_US 1200U
#define ALARM_REARM_GUARD_US 20U
#define ALL_CHANNELS_MASK ((1U << HEATER_CHANNEL_COUNT) - 1U)
#define EVENT_FRAME_START (1U << 0)
#define EVENT_CONVERT (1U << 1)
#define EVENT_ADC_READY (1U << 2)
#define EVENT_FRAME_DONE (1U << 3)
#define EVENT_ABORT (1U << 4)
#define EVENT_PAUSE (1U << 5)
#define EVENT_CONTROLLER_FAULT (1U << 6)

// 定义一个 20 ms 周期的加热、互锁、转换、关断和预充电阶段；ABORT 表示异常帧。
typedef enum {
  PHASE_START,
  PHASE_DISCHARGE,
  PHASE_ISOLATE,
  PHASE_MEASURE_ON,
  PHASE_CONVERT,
  PHASE_MEASURE_OFF,
  PHASE_DONE,
  PHASE_PRECHARGE,
  PHASE_COUNT,
  PHASE_ABORT,
} phase_t;

// 保存控制任务与 ISR 共用的周期、许可、运行和维护状态，驻留内部 RAM。
// 运行期共享状态用 lock 保护；暂停/恢复用 lifecycle 串行化，pause_ack
// 确认停止。
typedef struct {
  portMUX_TYPE lock;
  gptimer_handle_t timer;
  TaskHandle_t task;
  QueueHandle_t outbound;
  gptimer_alarm_config_t alarm;
  phase_t next_phase;
  phase_t phase;
  uint64_t cycle_start_us;
  uint64_t next_cycle_start_us;
  uint32_t sequence;
  uint8_t desired[HEATER_CHANNEL_COUNT];
  uint8_t applied[HEATER_CHANNEL_COUNT];
  heater_run_status_t run_status;
  uint32_t run_id;
  uint32_t frame_run_id;
  uint32_t generation, frame_generation;
  heater_runtime_run_callback_t on_run;
  bool bootstrap_ready;
  bool pwm_failed;
  uint8_t healthy_mask;
  uint32_t healthy_sequence;
  uint8_t started_mask;
  uint8_t ready_mask;
  uint64_t adc_start_us[HEATER_CHANNEL_COUNT];
  uint32_t faults;
  uint32_t deadline_misses;
  uint32_t telemetry_drops;
  heater_device_state_t device_state;
  bool board_ready;
  bool host_timeout;
  uint64_t lease_deadline_us;
  int64_t pause_start_us;
  SemaphoreHandle_t pause_ack;
  SemaphoreHandle_t lifecycle;
  heater_timing_stats_t timing;
  heater_mode_t mode;
  uint32_t config_id, ready_config_id, start_sequence, controller_faults;
  uint8_t algorithm_id;
  float target[4];
  heater_experiment_t applied_experiment;
  struct {
    uint32_t run_id, config_id, generation, apply_sequence, computation_us;
    controller_output_t output;
  } results[4];
  uint32_t worker_stack_free_bytes;
  uint32_t lease_refreshes;
  uint32_t controller_fault_generation, controller_fault_run_id;
} control_t;

extern control_t s_runtime;

// 保存一次短临界区读取的控制快照，供任务在锁外处理，可能随下一中断过期。
typedef struct {
  phase_t phase;
  uint32_t sequence;
  uint64_t cycle_start_us;
  uint8_t duty[HEATER_CHANNEL_COUNT];
  uint8_t ready_mask;
  uint32_t faults;
  heater_experiment_t experiment;
} snapshot_t;

// 所有下面的共享状态访问都使用 runtime lock；函数不得在持锁期间阻塞。
snapshot_t runtime_snapshot(void);
void runtime_add_fault(uint32_t sequence, uint32_t fault);
bool phase_alarm(gptimer_handle_t timer,
                 const gptimer_alarm_event_data_t* event, void* user_data);
void adc_ready(void* arg);
void start_conversion(uint32_t sequence);
void read_ready(heater_telemetry_t* frame, uint8_t* read_mask);
void publish(heater_telemetry_t* frame, bool aborted, uint8_t read_mask);
void control_task(void* arg);
void runtime_pwm_failure(void);
bool runtime_stop_locked(uint32_t fault);
extern uint8_t s_drdy_channel[HEATER_CHANNEL_COUNT];

#if !CONFIG_GPTIMER_ISR_CACHE_SAFE || !CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM || \
    !CONFIG_MCPWM_CTRL_FUNC_IN_IRAM || !CONFIG_GPIO_CTRL_FUNC_IN_IRAM
#error "Heater ISR requires cache-safe timer, PWM and GPIO functions"
#endif
