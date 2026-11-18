// Exercise the actual ISR/state logic with fake clocks, ADCs, GPIOs and queues.
// These tests cover event ordering, missed deadlines and fail-closed behavior;
// they do not simulate analog settling, hardware IRQ jitter or ADC accuracy.
#include <assert.h>
#include <math.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

#include "runtime_internal.h"

static uint64_t fake_now;
static bool heat_request, measure_request;
static uint8_t pwm[4];
static bool queue_full, pwm_ok;
static uint32_t driver_time_us;
static uint32_t adc_start_time_us;
static heater_message_t last_message;
static uint32_t sent_messages, notifications;
static int32_t adc_codes[4];
static bool drive_task;
static unsigned task_waits;
static unsigned task_frame_limit;
static bool check_hold;
static bool drive_pause, pause_ack, pause_timeout, timer_running;
static uint64_t stopped_counter;
static jmp_buf task_escape;

const int heater_pwm_gpio[4] = {4, 5, 6, 7};
const int heater_adc_cs_gpio[4] = {8, 9, 10, 14};
const int heater_adc_drdy_gpio[4] = {17, 18, 21, 38};
esp_err_t heater_board_init(void) { return ESP_OK; }
void heater_board_requests_off(void) { heat_request = measure_request = false; }
void heater_board_heat_request(bool enabled) {
  assert(!enabled || !measure_request);
  heat_request = enabled;
}
void heater_board_measure_request(bool enabled) {
  assert(!enabled || !heat_request);
  measure_request = enabled;
}
bool heater_board_pwm_low(void) {
  memset(pwm, 0, sizeof(pwm));
  fake_now += driver_time_us;
  return pwm_ok;
}
bool heater_board_pwm_apply_ticks(const uint8_t ticks[4]) {
  for (unsigned i = 0; i < 4; ++i) pwm[i] = ticks[i];
  return pwm_ok;
}
QueueHandle_t xQueueCreate(UBaseType_t count, UBaseType_t size) {
  (void)count;
  (void)size;
  return (void*)5;
}
BaseType_t xQueueReceive(QueueHandle_t q, void* out, TickType_t t) {
  (void)q;
  (void)out;
  (void)t;
  return pdFALSE;
}
BaseType_t xQueueOverwrite(QueueHandle_t q, const void* data) {
  (void)q;
  (void)data;
  return pdTRUE;
}
bool heater_board_pwm_apply(const uint8_t duty[4]) {
  memcpy(pwm, duty, 4);
  fake_now += driver_time_us;
  return pwm_ok;
}
esp_err_t ads1220_init(void) { return ESP_OK; }
esp_err_t ads1220_start(size_t channel) {
  (void)channel;
  fake_now += adc_start_time_us;
  return ESP_OK;
}
esp_err_t ads1220_read(size_t channel, int32_t* code) {
  *code = adc_codes[channel];
  fake_now += 40;
  return ESP_OK;
}
esp_err_t gptimer_get_raw_count(gptimer_handle_t timer, uint64_t* now) {
  (void)timer;
  *now = timer_running ? fake_now : stopped_counter;
  return ESP_OK;
}
esp_err_t gptimer_set_raw_count(gptimer_handle_t timer, uint64_t now) {
  (void)timer;
  stopped_counter = now;
  return ESP_OK;
}
int64_t esp_timer_get_time(void) { return (int64_t)fake_now; }
esp_err_t gptimer_set_alarm_action(gptimer_handle_t timer,
                                   const gptimer_alarm_config_t* alarm) {
  (void)timer;
  assert(alarm->alarm_count > fake_now);
  return ESP_OK;
}
esp_err_t gptimer_new_timer(const gptimer_config_t* config,
                            gptimer_handle_t* timer) {
  (void)config;
  *timer = (void*)1;
  return ESP_OK;
}
esp_err_t gptimer_register_event_callbacks(gptimer_handle_t timer,
                                           const gptimer_event_callbacks_t* cb,
                                           void* arg) {
  (void)timer;
  (void)cb;
  (void)arg;
  return ESP_OK;
}
esp_err_t gptimer_enable(gptimer_handle_t timer) {
  (void)timer;
  return ESP_OK;
}
esp_err_t gptimer_start(gptimer_handle_t timer) {
  (void)timer;
  timer_running = true;
  return ESP_OK;
}
esp_err_t gptimer_stop(gptimer_handle_t timer) {
  (void)timer;
  stopped_counter = fake_now;
  timer_running = false;
  return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t* config) {
  (void)config;
  return ESP_OK;
}
esp_err_t gpio_install_isr_service(int flags) {
  (void)flags;
  return ESP_OK;
}
esp_err_t gpio_isr_handler_add(int pin, void (*cb)(void*), void* arg) {
  (void)pin;
  (void)cb;
  (void)arg;
  return ESP_OK;
}
BaseType_t xTaskCreate(void (*entry)(void*), const char* name, uint32_t stack,
                       void* arg, UBaseType_t priority, TaskHandle_t* task) {
  (void)entry;
  (void)name;
  (void)stack;
  (void)arg;
  (void)priority;
  *task = (void*)1;
  return pdPASS;
}
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void*), const char* name,
                                   uint32_t stack, void* arg,
                                   UBaseType_t priority, TaskHandle_t* task,
                                   BaseType_t core) {
  assert(core == 0 || core == 1);
  return xTaskCreate(entry, name, stack, arg, priority, task);
}
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t bits, int action) {
  (void)task;
  (void)action;
  notifications |= bits;
  return pdPASS;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void*)3; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void*)2; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
  if (semaphore == (void*)3) pause_ack = true;
  return pdTRUE;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout) {
  if (semaphore == (void*)2) return pdTRUE;
  assert(semaphore == (void*)3);
  if (timeout && !pause_timeout && !pause_ack) {
    drive_pause = true;
    task_waits = 0;
    if (setjmp(task_escape) == 0) control_task(NULL);
    drive_pause = false;
  }
  const bool acknowledged = pause_ack;
  pause_ack = false;
  return acknowledged ? pdTRUE : pdFALSE;
}
BaseType_t xTaskNotifyFromISR(TaskHandle_t task, uint32_t bits, int action,
                              BaseType_t* woken) {
  (void)task;
  (void)action;
  notifications |= bits;
  *woken = pdFALSE;
  return pdPASS;
}
static void fire(uint32_t lateness);
BaseType_t xTaskNotifyWait(uint32_t entry, uint32_t exit_bits, uint32_t* bits,
                           TickType_t wait) {
  (void)entry;
  (void)exit_bits;
  (void)wait;
  if (drive_pause && task_waits++) longjmp(task_escape, 1);
  if (drive_task) {
    if (task_waits == task_frame_limit * 4) longjmp(task_escape, 1);
    switch (task_waits++ % 4) {
      case 0:
        if (check_hold) {
          const heater_command_t heartbeat = {.kind = HEATER_COMMAND_PING};
          assert(heater_runtime_submit(&heartbeat) == ESP_OK);
        }
        while (s_runtime.next_phase != PHASE_START) fire(0);
        fire(0);
        break;
      case 1:
        while (s_runtime.next_phase != PHASE_CONVERT) fire(0);
        fire(0);
        break;
      case 2:
        fake_now = s_runtime.cycle_start_us + 18650;
        for (size_t i = 0; i < 4; ++i) adc_ready(&s_drdy_channel[i]);
        break;
      case 3:
        while (s_runtime.next_phase != PHASE_DONE) fire(0);
        fire(0);
        break;
    }
  }
  *bits = notifications;
  notifications = 0;
  return pdPASS;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void* message,
                      TickType_t wait) {
  assert(wait == 0);
  if (queue_full) return pdFALSE;
  if (queue != s_runtime.outbound) return pdPASS;
  last_message = *(const heater_message_t*)message;
  if (check_hold && last_message.data.sequence > 1) {
    const uint8_t held_duty[4] = {5, 5, 5, 5};
    assert(memcmp(last_message.data.duty, held_duty, sizeof(held_duty)) == 0);
    assert(last_message.data.valid_mask == 15 && last_message.data.faults == 0);
  }
  ++sent_messages;
  return pdPASS;
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) {
  (void)task;
  return 2048;
}

static void reset(void) {
  memset(&s_runtime, 0, sizeof(s_runtime));
  s_runtime.timer = s_runtime.task = s_runtime.outbound = (void*)1;
  s_runtime.lifecycle = (void*)2;
  s_runtime.pause_ack = (void*)3;
  s_runtime.next_cycle_start_us = 20000;
  s_runtime.alarm.alarm_count = 20000;
  s_runtime.next_phase = PHASE_START;
  s_runtime.board_ready = true;
  s_runtime.device_state = HEATER_DEVICE_READY;
  assert(runtime_worker_start() == ESP_OK);
  fake_now = notifications = driver_time_us = sent_messages = 0;
  heat_request = measure_request = queue_full = false;
  pwm_ok = true;
  adc_start_time_us = 12;
  drive_task = false;
  task_waits = 0;
  task_frame_limit = 2;
  check_hold = false;
  drive_pause = pause_ack = pause_timeout = false;
  timer_running = true;
  memset(pwm, 0, sizeof(pwm));
  for (size_t i = 0; i < 4; ++i) adc_codes[i] = 3355443;
}

static void reset_recording(void) {
  reset();
  const heater_command_t start = {.kind = HEATER_COMMAND_START};
  assert(heater_runtime_submit(&start) == ESP_OK);
}

static void fire(uint32_t lateness) {
  const gptimer_alarm_event_data_t event = {
      .alarm_value = s_runtime.alarm.alarm_count,
      .count_value = s_runtime.alarm.alarm_count + lateness,
  };
  fake_now = event.count_value;
  phase_alarm(s_runtime.timer, &event, NULL);
  assert(!(heat_request && measure_request));
}

static heater_telemetry_t frame(void) {
  const snapshot_t current = runtime_snapshot();
  heater_telemetry_t data = {
      .sequence = current.sequence,
      .cycle_start_us = current.cycle_start_us,
      .resistance_ohm = {NAN, NAN, NAN, NAN},
  };
  memcpy(data.duty, current.duty, 4);
  return data;
}

static void reach_conversion(void) {
  while (s_runtime.next_phase != PHASE_CONVERT) fire(0);
  fire(0);
  start_conversion(s_runtime.sequence);
}

static void test_resistance_pipeline(void) {
  reset();
  controller_config_t config = {
      .algorithm = CONTROLLER_PID, .max_duty = 2, .tolerance_ohm = .05f};
  config.parameters[12] = .1f;
  config.parameters[13] = 1;
  for (unsigned i = 0; i < 4; ++i) {
    config.target[i] = 110;
    config.parameters[i] = .5f;
  }
  assert(heater_runtime_configure(&config) == ESP_OK &&
         !(s_runtime.run_status == HEATER_RUN_RUNNING));
  const heater_command_t start = {.kind = HEATER_COMMAND_RESISTANCE_START};
  const heater_command_t stop = {.kind = HEATER_COMMAND_STOP};
  s_runtime.ready_config_id = s_runtime.config_id;
  assert(heater_runtime_submit(&start) == ESP_OK);
  const uint64_t lease = s_runtime.lease_deadline_us;
  assert(heater_runtime_configure(&config) == ESP_ERR_INVALID_STATE);
  s_runtime.sequence = 2;
  s_runtime.healthy_sequence = 2;
  s_runtime.healthy_mask = 15;
  s_runtime.bootstrap_ready = true;
  s_runtime.next_cycle_start_us = 60000;
  s_runtime.alarm.alarm_count = 60000;
  s_runtime.results[3].run_id = s_runtime.run_id;
  s_runtime.results[3].config_id = s_runtime.config_id;
  s_runtime.results[3].apply_sequence = 3;
  s_runtime.results[3].output.source_sequence = 1;
  s_runtime.results[3].generation = s_runtime.generation;
  s_runtime.results[3].output.valid = true;
  for (unsigned i = 0; i < 4; ++i) s_runtime.results[3].output.ticks[i] = 3;
  fire(0);
  for (unsigned i = 0; i < 4; ++i)
    assert(pwm[i] == 3);  // 1.5 percent, not integer rounding.
  assert(s_runtime.applied_experiment.source_sequence == 1 &&
         s_runtime.lease_deadline_us == lease);
  s_runtime.next_phase = PHASE_START;
  s_runtime.next_cycle_start_us = 80000;
  s_runtime.alarm.alarm_count = 80000;
  s_runtime.healthy_sequence = 3;
  s_runtime.healthy_mask = 15;
  s_runtime.bootstrap_ready = true;
  fire(0);  // Missing result shuts down rather than retaining the old output.
  assert(!(s_runtime.run_status == HEATER_RUN_RUNNING) &&
         s_runtime.controller_faults == 1);
  for (unsigned i = 0; i < 4; ++i) assert(pwm[i] == 0);
  assert(heater_runtime_submit(&stop) == ESP_OK);
  assert(heater_runtime_submit(&start) == ESP_OK);
  const unsigned slot = (s_runtime.start_sequence + 2) % 4;
  s_runtime.results[slot].apply_sequence = s_runtime.start_sequence + 2;
  s_runtime.results[slot].run_id = s_runtime.run_id - 1;
  s_runtime.results[slot].config_id = s_runtime.config_id;
  s_runtime.sequence = s_runtime.start_sequence + 1;
  s_runtime.healthy_sequence = s_runtime.sequence;
  s_runtime.next_phase = PHASE_START;
  s_runtime.next_cycle_start_us = 160000;
  s_runtime.alarm.alarm_count = 160000;
  s_runtime.bootstrap_ready = true;
  fire(0);
  assert(!(s_runtime.run_status ==
           HEATER_RUN_RUNNING));  // A previous run cannot revive heating.
  puts(
      "Resistance pipeline, half-percent output, no auto-lease, deadlines and "
      "stale run rejection passed");
}

// STOP 打断应用编排后，旧 START 的恢复执行不得复活输出。
static void test_cancelled_start(void) {
  reset_recording();
  const uint32_t generation = heater_runtime_run_state().generation;
  assert(heater_runtime_stop() == ESP_OK);
  const heater_command_t start = {.kind = HEATER_COMMAND_START,
                                  .duty = {1, 1, 1, 1}};
  assert(heater_runtime_submit_in_generation(&start, generation) ==
         ESP_ERR_INVALID_STATE);
  assert(!heater_runtime_run_state().running && !heat_request);
}
// STOP 截断测量路径后，即使先前已读到数据也不能获得下一周期许可。
static void test_stopped_measurement(void) {
  reset_recording();
  fire(0);
  heater_telemetry_t sample = frame();
  sample.valid_mask = 15;
  for (unsigned i = 0; i < 4; ++i) sample.resistance_ohm[i] = 100;
  assert(heater_runtime_stop() == ESP_OK);
  publish(&sample, false, 15);
  assert(sample.valid_mask == 0 && s_runtime.healthy_mask == 0);
  assert(isnan(sample.resistance_ohm[0]));
}
// 无效算法结果只产生通知，实际关断由 Core 1 控制任务执行。
static void test_controller_fault_dispatch(void) {
  reset();
  s_runtime.mode = HEATER_MODE_RESISTANCE;
  s_runtime.config_id = s_runtime.ready_config_id = 2;
  const heater_command_t start = {.kind = HEATER_COMMAND_RESISTANCE_START,
                                  .config_id = 2};
  assert(heater_runtime_submit(&start) == ESP_OK);
  const controller_output_t invalid = {0};
  const heater_run_state_t state = heater_runtime_run_state();
  heater_runtime_controller_result(state.id, 2, state.generation - 1, 1,
                                   &invalid);
  assert(!(notifications & EVENT_CONTROLLER_FAULT));
  heater_runtime_controller_result(state.id, 2, state.generation, 1, &invalid);
  assert(heater_runtime_run_state().running &&
         (notifications & EVENT_CONTROLLER_FAULT));
  drive_pause = true;
  task_waits = 0;
  if (!setjmp(task_escape)) control_task(NULL);
  drive_pause = false;
  assert(!heater_runtime_run_state().running &&
         (s_runtime.faults & HEATER_FAULT_CONTROLLER));
}

// 手动命令覆盖完整接口范围，非法值不得替换已接受的设定。
static void test_manual_duty_range(void) {
  reset();
  const heater_command_t start = {.kind = HEATER_COMMAND_START,
                                  .duty = {3, 25, 50, 95}};
  assert(heater_runtime_submit(&start) == ESP_OK);
  const heater_command_t set = {.kind = HEATER_COMMAND_SET,
                                .duty = {95, 50, 25, 3}};
  assert(heater_runtime_submit(&set) == ESP_OK);
  for (unsigned i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
    heater_command_t invalid = set;
    invalid.duty[i] = HEATER_MAX_DUTY + 1;
    assert(heater_runtime_submit(&invalid) == ESP_ERR_INVALID_ARG);
    invalid.kind = HEATER_COMMAND_START;
    assert(heater_runtime_submit(&invalid) == ESP_ERR_INVALID_ARG);
    assert(memcmp(s_runtime.desired, set.duty, sizeof(set.duty)) == 0);
  }
}
// 恒电阻配置支持完整范围，95% 对应 190 个 PWM 比较器计数。
static void test_resistance_duty_range(void) {
  reset();
  controller_config_t config = {.algorithm = CONTROLLER_PID,
                                .max_duty = HEATER_MAX_DUTY,
                                .tolerance_ohm = .05f};
  config.parameters[12] = .1f;
  for (unsigned i = 0; i < 4; ++i) config.target[i] = 110;
  assert(heater_runtime_configure(&config) == ESP_OK);
  assert(heater_runtime_controller_status().config.max_duty == HEATER_MAX_DUTY);
  runtime_controller_ready(s_runtime.config_id, true);
  const heater_command_t start = {.kind = HEATER_COMMAND_RESISTANCE_START,
                                  .config_id = s_runtime.config_id};
  assert(heater_runtime_submit(&start) == ESP_OK);
  s_runtime.sequence = 2;
  s_runtime.healthy_sequence = 2;
  s_runtime.healthy_mask = 15;
  s_runtime.bootstrap_ready = true;
  s_runtime.next_phase = PHASE_START;
  s_runtime.next_cycle_start_us = s_runtime.alarm.alarm_count = 60000;
  controller_output_t output = {.source_sequence = 1, .valid = true};
  for (unsigned i = 0; i < 4; ++i) {
    output.requested[i] = HEATER_MAX_DUTY;
    output.ticks[i] = HEATER_MAX_DUTY * 2U;
  }
  heater_runtime_controller_result(s_runtime.run_id, s_runtime.config_id,
                                   s_runtime.generation, 10, &output);
  fire(0);
  assert(heat_request);
  for (unsigned i = 0; i < 4; ++i) assert(pwm[i] == HEATER_MAX_DUTY * 2U);
  assert(heater_runtime_stop() == ESP_OK);
  config.max_duty = HEATER_MAX_DUTY + .5f;
  assert(heater_runtime_configure(&config) == ESP_ERR_INVALID_ARG);
}

int main(void) {
  test_manual_duty_range();
  test_resistance_duty_range();
  test_resistance_pipeline();
  test_cancelled_start();
  test_stopped_measurement();
  test_controller_fault_dispatch();
  // Idle safety sampling must not enqueue any historical samples. SET stages
  // duties without enabling heat/recording, and PING cannot start an idle run.
  reset();
  const heater_command_t staged = {.kind = HEATER_COMMAND_SET,
                                   .duty = {2, 1, 0, 2}};
  assert(heater_runtime_submit(&staged) == ESP_OK);
  assert(!heater_runtime_run_state().running);
  drive_task = true;
  task_frame_limit = 4;
  if (setjmp(task_escape) == 0) control_task(NULL);
  drive_task = false;
  heater_telemetry_t private_sample;
  assert(sent_messages == 0 && !heat_request);
  assert(heater_runtime_latest(&private_sample) &&
         private_sample.valid_mask == 15);
  // An explicit zero-duty START records; STOP closes that run and idle
  // samples keep the safety state fresh without growing the output queue.
  const heater_command_t start_zero = {.kind = HEATER_COMMAND_START};
  const heater_command_t stop_zero = {.kind = HEATER_COMMAND_STOP};
  assert(heater_runtime_submit(&start_zero) == ESP_OK);
  const uint32_t first_run = heater_runtime_run_state().id;
  task_waits = 0;
  task_frame_limit = 3;
  drive_task = true;
  if (setjmp(task_escape) == 0) control_task(NULL);
  drive_task = false;
  assert(sent_messages == 3 && last_message.run_id == first_run);
  assert(heater_runtime_submit(&stop_zero) == ESP_OK);
  task_waits = 0;
  task_frame_limit = 3;
  drive_task = true;
  if (setjmp(task_escape) == 0) control_task(NULL);
  drive_task = false;
  assert(sent_messages == 3 && !heater_runtime_run_state().running);
  assert(heater_runtime_submit(&start_zero) == ESP_OK);
  assert(heater_runtime_run_state().id != first_run);

  // Real-board timings: 24 us to wake the task, 34 us per software-CS START.
  // The old 100 us window stopped before ADC4 and invalidated all readings.
  reset_recording();
  adc_start_time_us = 34;
  fire(0);
  while (s_runtime.next_phase != PHASE_CONVERT) fire(0);
  fire(0);
  fake_now += 24;
  start_conversion(s_runtime.sequence);
  assert(s_runtime.started_mask == 15 && s_runtime.faults == 0);
  assert(fake_now == s_runtime.cycle_start_us + ADC_START_US + 160);
  fake_now = s_runtime.cycle_start_us + 18750;
  for (size_t i = 0; i < 4; ++i) adc_ready(&s_drdy_channel[i]);
  heater_telemetry_t actual_timing = frame();
  uint8_t actual_read_mask = 0;
  read_ready(&actual_timing, &actual_read_mask);
  fire(0);
  fire(0);
  publish(&actual_timing, false, actual_read_mask);
  assert(last_message.data.valid_mask == 15 && last_message.data.faults == 0);

  // A command that starts in time but finishes too late must still be rejected.
  reset_recording();
  fire(0);
  while (s_runtime.next_phase != PHASE_CONVERT) fire(0);
  fire(0);
  fake_now = s_runtime.cycle_start_us + ADC_START_DEADLINE_US - 5;
  start_conversion(s_runtime.sequence);
  assert(s_runtime.started_mask == 0 &&
         (s_runtime.faults & HEATER_FAULT_TIMING));

  reset_recording();
  heater_command_t set = {.kind = HEATER_COMMAND_START,
                          .duty = {1, 25, 50, 95}};
  assert(heater_runtime_submit(&set) == ESP_OK);
  fire(0);
  assert(
      !heat_request);  // A command alone does not bypass initial measurement.
  heater_telemetry_t first = frame();
  reach_conversion();
  // Old DRDY edges left over from initialization must not qualify.
  adc_ready(&s_drdy_channel[0]);
  assert(s_runtime.ready_mask == 0);
  fake_now = first.cycle_start_us + 18650;
  for (size_t i = 0; i < 4; ++i) adc_ready(&s_drdy_channel[i]);
  assert(s_runtime.ready_mask == 15);
  uint8_t read_mask = 0;
  read_ready(&first, &read_mask);
  assert(first.valid_mask == 15 && read_mask == 15);
  fire(0);  // Measurement cutoff still occurs without any task response.
  assert(!measure_request && s_runtime.started_mask == 0);
  fire(0);  // Done
  publish(&first, false, read_mask);
  assert(last_message.data.valid_mask == 15);
  fire(0);  // Precharge
  assert(heat_request && pwm[0] == 0 && pwm[3] == 0);
  assert(runtime_snapshot().cycle_start_us ==
         first.cycle_start_us);  // Frame identity stays stable.
  fire(0);                       // Next frame
  for (unsigned i = 0; i < 4; ++i) assert(pwm[i] == set.duty[i] * 2U);
  assert(heat_request);
  assert(s_runtime.cycle_start_us == 40000);
  const heater_command_t stop = {.kind = HEATER_COMMAND_STOP};
  assert(heater_runtime_submit(&stop) == ESP_OK);
  assert(!heat_request && (s_runtime.faults & HEATER_FAULT_STOPPED));

  // Missing one converter inhibits only that channel after the next frame.
  reset_recording();
  assert(heater_runtime_submit(&set) == ESP_OK);
  fire(0);
  heater_telemetry_t partial = frame();
  reach_conversion();
  fake_now = partial.cycle_start_us + 18650;
  for (size_t i = 0; i < 3; ++i) adc_ready(&s_drdy_channel[i]);
  read_mask = 0;
  read_ready(&partial, &read_mask);
  fire(0);
  fire(0);
  publish(&partial, false, read_mask);
  assert(partial.valid_mask == 7 &&
         (partial.faults & HEATER_FAULT_ADC_TIMEOUT));
  fire(0);
  fire(0);
  assert(pwm[0] == 2 && pwm[1] == 50 && pwm[2] == 100 && pwm[3] == 0);

  // Malformed/saturated data is a sample fault, not a DRDY timeout.
  reset_recording();
  fire(0);
  heater_telemetry_t bad_sample = frame();
  reach_conversion();
  adc_codes[2] = 0x7fffff;
  fake_now = bad_sample.cycle_start_us + 18650;
  for (size_t i = 0; i < 4; ++i) adc_ready(&s_drdy_channel[i]);
  read_mask = 0;
  read_ready(&bad_sample, &read_mask);
  fire(0);
  fire(0);
  publish(&bad_sample, false, read_mask);
  assert(bad_sample.valid_mask == 11 && isnan(bad_sample.resistance_ohm[2]));
  assert(bad_sample.faults & HEATER_FAULT_SAMPLE);
  assert(!(bad_sample.faults & HEATER_FAULT_ADC_TIMEOUT));

  // USB backpressure never prevents hardware measurement cutoff.
  reset_recording();
  fire(0);
  heater_telemetry_t dropped = frame();
  reach_conversion();
  queue_full = true;
  fire(0);
  assert(!measure_request);
  fire(0);
  publish(&dropped, false, 0);
  assert(s_runtime.telemetry_drops == 1);
  assert(s_runtime.alarm.alarm_count == 39900);

  // An IRQ arriving several frames late must schedule a future alarm.
  reset_recording();
  fire(45000);
  assert(s_runtime.phase == PHASE_ABORT &&
         s_runtime.alarm.alarm_count == 80000);
  assert(!heat_request && !measure_request);
  assert(s_runtime.deadline_misses == 1);
  // A callback that enters on time can also overrun its next phase.
  reset_recording();
  fire(0);
  driver_time_us = 150;
  fire(0);
  assert(s_runtime.phase == PHASE_ABORT && !heat_request);
  assert(s_runtime.alarm.alarm_count > fake_now);
  // Even an unexpectedly long callback must not schedule an expired alarm.
  reset_recording();
  fire(0);
  driver_time_us = 50000;
  fire(0);
  assert(s_runtime.phase == PHASE_ABORT && !heat_request && !measure_request);
  assert(s_runtime.alarm.alarm_count == 100000);

  // If the control task never finishes a frame, cached healthy readings
  // cannot keep heating enabled at the next boundary.
  reset_recording();
  s_runtime.healthy_mask = 15;
  s_runtime.bootstrap_ready = true;
  assert(heater_runtime_submit(&set) == ESP_OK);
  fire(0);
  assert(heat_request);
  while (s_runtime.next_phase != PHASE_START) fire(0);
  fire(0);
  assert(!heat_request && (s_runtime.faults & HEATER_FAULT_TIMING));

  // Exercise the real notification loop over two complete frames.
  reset_recording();
  assert(heater_runtime_submit(&set) == ESP_OK);
  drive_task = true;
  if (setjmp(task_escape) == 0) control_task(NULL);
  drive_task = false;
  assert(sent_messages == 2);
  assert(last_message.data.sequence == 2 &&
         last_message.data.cycle_start_us == 40000);
  assert(last_message.data.valid_mask == 15 && last_message.data.faults == 0);
  assert(memcmp(last_message.data.duty, set.duty, 4) == 0);

  // A START renewed by PING holds through 3001 notification-loop frames (>60
  // s).
  reset();
  const heater_command_t ping = {.kind = HEATER_COMMAND_PING};
  assert(heater_runtime_submit(&ping) == ESP_OK);
  assert(!heat_request && !(s_runtime.run_status == HEATER_RUN_RUNNING));
  const heater_command_t hold = {.kind = HEATER_COMMAND_START,
                                 .duty = {5, 5, 5, 5}};
  assert(heater_runtime_submit(&hold) == ESP_OK);
  task_frame_limit = 3001;
  check_hold = drive_task = true;
  if (setjmp(task_escape) == 0) control_task(NULL);
  check_hold = drive_task = false;
  assert(sent_messages == 3001 && last_message.data.cycle_start_us == 60020000);
  assert(last_message.data.deadline_misses == 0 &&
         last_message.data.telemetry_drops == 0);
  assert(heater_runtime_submit(&ping) == ESP_OK);
  assert((s_runtime.run_status == HEATER_RUN_RUNNING) &&
         memcmp(s_runtime.desired, hold.duty, 4) == 0);

  // A new SET replaces the held values; STOP cancels them, and PING cannot
  // restart heating even when measurements would permit it.
  const heater_command_t replacement = {.kind = HEATER_COMMAND_SET,
                                        .duty = {0, 5, 0, 5}};
  assert(heater_runtime_submit(&replacement) == ESP_OK);
  assert((s_runtime.run_status == HEATER_RUN_RUNNING) &&
         memcmp(s_runtime.desired, replacement.duty, 4) == 0);
  assert(heater_runtime_submit(&stop) == ESP_OK);
  assert(!heat_request && !(s_runtime.run_status == HEATER_RUN_RUNNING));
  const uint8_t zero_duty[4] = {0};
  assert(memcmp(s_runtime.desired, zero_duty, 4) == 0);
  assert(heater_runtime_submit(&ping) == ESP_OK);
  assert(!heat_request && !(s_runtime.run_status == HEATER_RUN_RUNNING));
  fire(0);  // Precharge with held-low PWM.
  fire(0);  // Next frame applies STOP's zero duty.
  assert(!heat_request && memcmp(pwm, zero_duty, 4) == 0);
  // Lease expiry runs in the phase ISR even if the network tasks are blocked.
  reset_recording();
  assert(heater_runtime_submit(&set) == ESP_OK);
  const uint64_t expiry = s_runtime.lease_deadline_us;
  s_runtime.next_cycle_start_us = expiry;
  s_runtime.alarm.alarm_count = expiry;
  fire(0);
  assert(!(s_runtime.run_status == HEATER_RUN_RUNNING) && !heat_request);
  assert(s_runtime.faults & HEATER_FAULT_HOST_TIMEOUT);
  assert(heater_runtime_submit(&ping) == ESP_OK &&
         !(s_runtime.run_status == HEATER_RUN_RUNNING));

  // Maintenance acknowledgements execute the real task branch, then prohibit
  // GPIO phase activity, ADC starts and SET/PING until an explicit resume.
  reset_recording();
  assert(heater_runtime_submit(&set) == ESP_OK);
  fire(0);
  reach_conversion();
  assert(measure_request);
  assert(heater_runtime_pause(pdMS_TO_TICKS(2000)) == ESP_OK);
  assert((s_runtime.device_state == HEATER_DEVICE_PAUSED) && !timer_running &&
         !heat_request && !measure_request);
  assert(heater_runtime_submit(&set) == ESP_ERR_INVALID_STATE);
  assert(heater_runtime_submit(&ping) == ESP_ERR_INVALID_STATE);
  phase_alarm(s_runtime.timer, &(gptimer_alarm_event_data_t){0}, NULL);
  assert(!heat_request && !measure_request);
  assert(heater_runtime_pause(pdMS_TO_TICKS(2000)) == ESP_OK);
  fake_now += 2000000;
  assert(heater_runtime_resume() == ESP_OK);
  assert(!(s_runtime.device_state == HEATER_DEVICE_PAUSING ||
           s_runtime.device_state == HEATER_DEVICE_PAUSED) &&
         timer_running && !(s_runtime.run_status == HEATER_RUN_RUNNING));
  assert(s_runtime.alarm.alarm_count > fake_now);
  fire(0);
  assert(!heat_request && memcmp(pwm, zero_duty, 4) == 0);

  // An acknowledgement timeout never grants permission to write flash.
  reset();
  pause_timeout = true;
  assert(heater_runtime_pause(pdMS_TO_TICKS(2000)) == ESP_ERR_TIMEOUT);
  assert((s_runtime.device_state == HEATER_DEVICE_PAUSING ||
          s_runtime.device_state == HEATER_DEVICE_PAUSED) &&
         !(s_runtime.device_state == HEATER_DEVICE_PAUSED) && !heat_request &&
         !measure_request);
  assert(!heater_runtime_is_paused() && heater_runtime_is_maintenance());
  assert(heater_runtime_resume() == ESP_ERR_INVALID_STATE);
  // A failed ADC initialization leaves no task/timer. It still permits a
  // verified output shutdown so web/OTA can recover the device without ADCs.
  reset();
  s_runtime.task = s_runtime.timer = NULL;
  s_runtime.board_ready = false;
  assert(heater_runtime_pause(pdMS_TO_TICKS(1000)) == ESP_ERR_INVALID_STATE);
  s_runtime.board_ready = true;
  assert(heater_runtime_pause(pdMS_TO_TICKS(1000)) == ESP_OK);
  assert(heater_runtime_is_paused() && !heat_request && !measure_request);
  assert(heater_runtime_submit(&stop) == ESP_OK);
  assert(heater_runtime_submit(&set) == ESP_ERR_INVALID_STATE);
  assert(heater_runtime_resume() == ESP_ERR_INVALID_STATE);
  puts("Control timing, shutdown, ADC event and backpressure tests passed");
  return 0;
}
