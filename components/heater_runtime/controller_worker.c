// Core 0 算法任务只使用值类型和结果接口，禁止访问板级输出。
#include "controller_worker.h"

#include <math.h>
#include <string.h>

#include "esp_timer.h"
#include "freertos/task.h"
#include "sdkconfig.h"
typedef struct {
  controller_config_t config;
  uint32_t config_id;
} config_job_t;
typedef struct {
  heater_telemetry_t frame;
  uint32_t run_id, generation;
} frame_job_t;
static struct {
  portMUX_TYPE lock;
  QueueHandle_t configurations, frames;
  TaskHandle_t task;
  controller_config_t requested;
  heater_config_state_t state;
} s_worker = {.lock = portMUX_INITIALIZER_UNLOCKED};

// 配置先处理；运行数据覆盖只影响 frames 邮箱，不覆盖配置请求。
static void controller_task(void* arg) {
  (void)arg;
  config_job_t config = {0};
  uint32_t run_id = 0;
  bool configured = false;
  for (;;) {
    uint32_t events;
    xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
    config_job_t incoming;
    if (xQueueReceive(s_worker.configurations, &incoming, 0) == pdTRUE) {
      config = incoming;
      configured = heater_controller_configure(&config.config);
      portENTER_CRITICAL(&s_worker.lock);
      s_worker.state = configured ? HEATER_CONFIG_READY : HEATER_CONFIG_FAILED;
      portEXIT_CRITICAL(&s_worker.lock);
      runtime_controller_ready(config.config_id, configured);
    }
    frame_job_t job;
    if (xQueueReceive(s_worker.frames, &job, 0) != pdTRUE) continue;
    // 陈旧帧不能驱动新的算法状态，检查后仍由结果接口再次核验代次。
    if (job.frame.experiment.config_id != config.config_id) continue;
    const int64_t begin = esp_timer_get_time();
    if (run_id != job.run_id) {
      heater_controller_reset();
      run_id = job.run_id;
    }
    controller_applied_t applied = {
        .source_sequence = job.frame.experiment.source_sequence,
        .blocked_mask = job.frame.experiment.blocked_mask,
        .limited_mask = job.frame.experiment.limited_mask};
    for (unsigned i = 0; i < 4; ++i) {
      applied.requested[i] = job.frame.experiment.requested[i];
      applied.applied[i] =
          (job.frame.faults &
           (HEATER_FAULT_STOPPED | HEATER_FAULT_TIMING | HEATER_FAULT_PWM))
              ? 0
              : job.frame.experiment.pwm_ticks[i] * 0.5f;
    }
    heater_controller_on_applied(&applied);
    controller_input_t input = {.sequence = job.frame.sequence,
                                .sample_time_us = job.frame.cycle_start_us,
                                .valid_mask = job.frame.valid_mask};
    memcpy(input.resistance, job.frame.resistance_ohm,
           sizeof(input.resistance));
    controller_output_t output =
        configured ? heater_controller_step(&input) : (controller_output_t){0};
    for (unsigned i = 0; i < 4; ++i)
      if (!isfinite(output.requested[i]) ||
          output.ticks[i] > HEATER_MAX_DUTY * 2U ||
          output.ticks[i] > config.config.max_duty * 2.0f)
        output.valid = false;
    heater_runtime_controller_result(
        job.run_id, config.config_id, job.generation,
        (uint32_t)(esp_timer_get_time() - begin), &output);
  }
}
esp_err_t runtime_worker_start(void) {
  s_worker.configurations = xQueueCreate(1, sizeof(config_job_t));
  s_worker.frames = xQueueCreate(1, sizeof(frame_job_t));
  if (!s_worker.configurations || !s_worker.frames) return ESP_ERR_NO_MEM;
  if (xTaskCreatePinnedToCore(controller_task, "heater_algorithm", 6144, NULL,
                              7, &s_worker.task, 0) != pdPASS)
    return ESP_ERR_NO_MEM;
  return ESP_OK;
}
bool runtime_worker_configure(const controller_config_t* config,
                              uint32_t config_id) {
  const config_job_t job = {.config = *config, .config_id = config_id};
  portENTER_CRITICAL(&s_worker.lock);
  if (xQueueSend(s_worker.configurations, &job, 0) != pdTRUE) {
    portEXIT_CRITICAL(&s_worker.lock);
    return false;
  }
  s_worker.requested = *config;
  s_worker.state = HEATER_CONFIG_PREPARING;
  portEXIT_CRITICAL(&s_worker.lock);
  xTaskNotify(s_worker.task, 1, eSetBits);
  return true;
}
void runtime_worker_frame(const heater_telemetry_t* frame, uint32_t run_id,
                          uint32_t generation) {
  const frame_job_t job = {
      .frame = *frame, .run_id = run_id, .generation = generation};
  xQueueOverwrite(s_worker.frames, &job);
  xTaskNotify(s_worker.task, 2, eSetBits);
}
controller_config_t runtime_worker_config(void) {
  portENTER_CRITICAL(&s_worker.lock);
  const controller_config_t config = s_worker.requested;
  portEXIT_CRITICAL(&s_worker.lock);
  return config;
}

heater_config_state_t runtime_worker_state(void) {
  portENTER_CRITICAL(&s_worker.lock);
  const heater_config_state_t state = s_worker.state;
  portEXIT_CRITICAL(&s_worker.lock);
  return state;
}
