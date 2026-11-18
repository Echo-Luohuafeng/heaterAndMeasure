// 保留已验证的时序；所有硬件操作属于实时执行组件。
#include <math.h>

#include "ads1220.h"
#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "runtime_internal.h"
control_t s_runtime = {.lock = portMUX_INITIALIZER_UNLOCKED};
uint8_t s_drdy_channel[HEATER_CHANNEL_COUNT] = {0, 1, 2, 3};
// 状态查询只读取此观察快照，阶段 ISR 不接触它。
static struct {
  portMUX_TYPE lock;
  bool present;
  heater_telemetry_t frame;
  int64_t published_us;
} s_observation = {.lock = portMUX_INITIALIZER_UNLOCKED};
static const char* TAG = "heater_runtime";
// 在短临界区内复制阶段、周期及通道状态，供任务在锁外处理采样。
snapshot_t runtime_snapshot(void) {
  snapshot_t result;
  portENTER_CRITICAL(&s_runtime.lock);
  result.phase = s_runtime.phase;
  result.sequence = s_runtime.sequence;
  result.cycle_start_us = s_runtime.cycle_start_us;
  memcpy(result.duty, s_runtime.applied, sizeof(result.duty));
  result.ready_mask = s_runtime.ready_mask;
  result.faults = s_runtime.faults;
  result.experiment = s_runtime.applied_experiment;
  portEXIT_CRITICAL(&s_runtime.lock);
  return result;
}

// 汇总 `frame` 的故障，按 `aborted` 和 `read_mask` 判定有效性并更新下周期许可。
// 无效电阻写为 NAN；内部安全快照始终更新，只有当前显式运行才非阻塞入队。
// `frame` 须非空；跨周期结果丢弃，队列满仅累计丢帧，不等待缓存或网络。
void publish(heater_telemetry_t* frame, bool aborted, uint8_t read_mask) {
  portENTER_CRITICAL(&s_runtime.lock);
  if (s_runtime.sequence != frame->sequence) {
    portEXIT_CRITICAL(&s_runtime.lock);
    return;
  }
  frame->faults |= s_runtime.faults;
  if (read_mask != ALL_CHANNELS_MASK) {
    frame->faults |= HEATER_FAULT_ADC_TIMEOUT;
  }
  if (aborted ||
      (frame->faults & (HEATER_FAULT_TIMING | HEATER_FAULT_PWM |
                        HEATER_FAULT_STOPPED | HEATER_FAULT_CONTROLLER))) {
    frame->valid_mask = 0;
  }
  s_runtime.healthy_mask = frame->valid_mask;
  s_runtime.healthy_sequence = frame->sequence;
  frame->deadline_misses = s_runtime.deadline_misses;
  frame->telemetry_drops = s_runtime.telemetry_drops;
  portEXIT_CRITICAL(&s_runtime.lock);
  for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
    if (!(frame->valid_mask & (1U << i))) {
      frame->resistance_ohm[i] = NAN;
    }
  }
  frame->control_stack_free_bytes = uxTaskGetStackHighWaterMark(NULL);
  portENTER_CRITICAL(&s_runtime.lock);
  if (s_runtime.sequence != frame->sequence) {
    portEXIT_CRITICAL(&s_runtime.lock);
    return;
  }
  const bool record = (s_runtime.run_status == HEATER_RUN_RUNNING) &&
                      s_runtime.frame_run_id != 0 &&
                      s_runtime.frame_run_id == s_runtime.run_id;
  const uint32_t run_id = s_runtime.frame_run_id;
  const uint32_t generation = s_runtime.frame_generation;
  portEXIT_CRITICAL(&s_runtime.lock);
  portENTER_CRITICAL(&s_observation.lock);
  s_observation.frame = *frame;
  s_observation.present = true;
  s_observation.published_us = esp_timer_get_time();
  portEXIT_CRITICAL(&s_observation.lock);
  // 运行与代次来自同一次快照；STOP 后旧帧在接收门处被拒绝。
  if (record && frame->experiment.mode == HEATER_MODE_RESISTANCE)
    runtime_worker_frame(frame, run_id, generation);
  if (!record) return;
  const heater_message_t message = {
      .run_id = run_id, .generation = generation, .data = *frame};
  if (xQueueSend(s_runtime.outbound, &message, 0) != pdPASS) {
    portENTER_CRITICAL(&s_runtime.lock);
    ++s_runtime.telemetry_drops;
    portEXIT_CRITICAL(&s_runtime.lock);
  }
}

// 阻塞等待阶段、DRDY 和维护通知，在任务上下文执行 SPI、换算及发布。
// 维护时停止 GPTimer、确认 PWM 为低后应答；每个周期最多发布一次。
// `arg` 未使用；heater_cycle 位于 Core 1，优先级 20，栈为 6144 字节。
void control_task(void* arg) {
  (void)arg;
  heater_telemetry_t frame = {0};
  uint8_t read_mask = 0;
  uint32_t published_sequence = 0;
  for (;;) {
    uint32_t events;
    xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
    portENTER_CRITICAL(&s_runtime.lock);
    const bool maintenance = (s_runtime.device_state == HEATER_DEVICE_PAUSING ||
                              s_runtime.device_state == HEATER_DEVICE_PAUSED);
    portEXIT_CRITICAL(&s_runtime.lock);
    if (maintenance) {
      if (events & EVENT_PAUSE) {
        const esp_err_t stopped = gptimer_stop(s_runtime.timer);
        portENTER_CRITICAL(&s_runtime.lock);
        heater_board_requests_off();
        const bool low = heater_board_pwm_low();
        s_runtime.device_state = stopped == ESP_OK && low
                                     ? HEATER_DEVICE_PAUSED
                                     : HEATER_DEVICE_PAUSING;
        s_runtime.pause_start_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_runtime.lock);
        xSemaphoreGive(s_runtime.pause_ack);
      }
      continue;
    }
    // 算法任务只报告故障；Core 1 核验身份后执行关断。
    if (events & EVENT_CONTROLLER_FAULT) {
      portENTER_CRITICAL(&s_runtime.lock);
      if (s_runtime.run_status == HEATER_RUN_RUNNING &&
          s_runtime.controller_fault_generation == s_runtime.generation &&
          s_runtime.controller_fault_run_id == s_runtime.run_id) {
        runtime_stop_locked(HEATER_FAULT_CONTROLLER);
        ++s_runtime.controller_faults;
      }
      portEXIT_CRITICAL(&s_runtime.lock);
    }
    const snapshot_t current = runtime_snapshot();
    if (frame.sequence != current.sequence) {
      frame = (heater_telemetry_t){
          .sequence = current.sequence,
          .cycle_start_us = current.cycle_start_us,
          .resistance_ohm = {NAN, NAN, NAN, NAN},
      };
      memcpy(frame.duty, current.duty, sizeof(frame.duty));
      frame.experiment = current.experiment;
      read_mask = 0;
    }
    if ((events & EVENT_CONVERT) && current.phase == PHASE_CONVERT) {
      start_conversion(frame.sequence);
    }
    if (events & EVENT_ADC_READY) {
      read_ready(&frame, &read_mask);
    }
    const snapshot_t ending = runtime_snapshot();
    const bool aborted = (events & EVENT_ABORT) && ending.phase == PHASE_ABORT;
    const bool done =
        (events & EVENT_FRAME_DONE) &&
        (ending.phase == PHASE_DONE || ending.phase == PHASE_PRECHARGE);
    if ((done || aborted) && published_sequence != frame.sequence) {
      publish(&frame, aborted, read_mask);
      published_sequence = frame.sequence;
    }
  }
}

// 创建暂停应答和生命周期锁，再依次初始化板级输出、ADC、GPTimer 与 DRDY ISR。
// 中断归属调用所在核心；ADC 配置失败时关闭请求并保留板级恢复能力。
esp_err_t heater_runtime_init(QueueHandle_t outbound) {
  if (outbound == NULL || s_runtime.outbound != NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  s_runtime.outbound = outbound;
  s_runtime.pause_ack = xSemaphoreCreateBinary();
  s_runtime.lifecycle = xSemaphoreCreateMutex();
  if (s_runtime.pause_ack == NULL || s_runtime.lifecycle == NULL) {
    return ESP_ERR_NO_MEM;
  }
  ESP_RETURN_ON_ERROR(heater_board_init(), TAG, "Board initialization failed");
  s_runtime.board_ready = true;
  const esp_err_t adc_result = ads1220_init();
  if (adc_result != ESP_OK) {
    heater_board_requests_off();
    return adc_result;
  }
  const gptimer_config_t timer = {
      .clk_src = GPTIMER_CLK_SRC_DEFAULT,
      .direction = GPTIMER_COUNT_UP,
      .resolution_hz = 1000000,
      .intr_priority = 3,
  };
  ESP_RETURN_ON_ERROR(gptimer_new_timer(&timer, &s_runtime.timer), TAG,
                      "Phase timer failed");
  const gptimer_event_callbacks_t callbacks = {.on_alarm = phase_alarm};
  ESP_RETURN_ON_ERROR(
      gptimer_register_event_callbacks(s_runtime.timer, &callbacks, NULL), TAG,
      "Phase callback failed");
  const gpio_config_t drdy = {
      .pin_bit_mask = (1ULL << 17) | (1ULL << 18) | (1ULL << 21) | (1ULL << 38),
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .intr_type = GPIO_INTR_NEGEDGE,
  };
  ESP_RETURN_ON_ERROR(gpio_config(&drdy), TAG, "DRDY GPIO failed");
  ESP_RETURN_ON_ERROR(
      gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3), TAG,
      "DRDY ISR service failed");
  for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
    ESP_RETURN_ON_ERROR(gpio_isr_handler_add(heater_adc_drdy_gpio[i], adc_ready,
                                             &s_drdy_channel[i]),
                        TAG, "DRDY callback failed");
  }
  return ESP_OK;
}

// 创建与阶段中断同核的高优先级控制任务，安排首个绝对周期闹钟后启动计时。
esp_err_t heater_runtime_start(void) {
  if (s_runtime.timer == NULL || s_runtime.task != NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  ESP_RETURN_ON_ERROR(runtime_worker_start(), TAG,
                      "Algorithm worker creation failed");
  s_runtime.config_id = 1;
  // 启动初始化及中断分配同样位于 Core 1，见 app_main。
  if (xTaskCreatePinnedToCore(
          control_task, "heater_cycle", CONTROL_TASK_STACK_BYTES, NULL,
          CONTROL_TASK_PRIORITY, &s_runtime.task, 1) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }
  s_runtime.next_phase = PHASE_START;
  s_runtime.next_cycle_start_us = HEATER_PERIOD_US;
  s_runtime.alarm.alarm_count = HEATER_PERIOD_US;
  ESP_RETURN_ON_ERROR(
      gptimer_set_alarm_action(s_runtime.timer, &s_runtime.alarm), TAG,
      "Initial phase alarm failed");
  ESP_RETURN_ON_ERROR(gptimer_enable(s_runtime.timer), TAG,
                      "Phase timer enable failed");
  ESP_LOGI(TAG,
           "20ms frame, 15ms heat; 10kHz PWM; ADS1220 Turbo 660SPS, gain 4, "
           "IDAC 1.5mA");
  esp_err_t result = gptimer_start(s_runtime.timer);
  if (result == ESP_OK) s_runtime.device_state = HEATER_DEVICE_READY;
  return result;
}

// 当前测量独立于历史，age 表示发布后的单调时间；维护时可识别陈旧样本。
bool heater_runtime_latest(heater_telemetry_t* frame) {
  if (!frame) return false;
  portENTER_CRITICAL(&s_observation.lock);
  const bool present = s_observation.present;
  if (present) *frame = s_observation.frame;
  portEXIT_CRITICAL(&s_observation.lock);
  return present;
}
uint64_t heater_runtime_measurement_age_us(void) {
  portENTER_CRITICAL(&s_observation.lock);
  const uint64_t age =
      s_observation.present
          ? (uint64_t)(esp_timer_get_time() - s_observation.published_us)
          : UINT64_MAX;
  portEXIT_CRITICAL(&s_observation.lock);
  return age;
}
