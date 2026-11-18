// 保留已验证的时序；所有硬件操作属于实时执行组件。
#include "runtime_internal.h"
static DRAM_ATTR uint32_t s_phase_us[PHASE_COUNT] = {
    0, 15000, 15100, 15200, ADC_START_US, MEASURE_OFF_US, 19350, 19900};
// 在 GPTimer ISR 中推进绝对阶段时序，切换请求/PWM 并通知控制任务。
// `timer`、`event` 由驱动提供且非空，`user_data` 未使用；返回是否需切换任务。
// 迟到或重设闹钟失败时立即关断并丢弃该帧，恢复闹钟只安排到未来边界。
// 仅执行有界的 IRAM 操作与 FromISR 通知，不在中断中进行 SPI、日志或换算。
bool IRAM_ATTR phase_alarm(gptimer_handle_t timer,
                           const gptimer_alarm_event_data_t* event,
                           void* user_data) {
  (void)user_data;
  uint32_t notification = 0;
  BaseType_t woken = pdFALSE;
  portENTER_CRITICAL_ISR(&s_runtime.lock);
  if ((s_runtime.device_state == HEATER_DEVICE_PAUSING ||
       s_runtime.device_state == HEATER_DEVICE_PAUSED)) {
    heater_board_requests_off();
    portEXIT_CRITICAL_ISR(&s_runtime.lock);
    return false;
  }
  const phase_t phase = s_runtime.next_phase;
  s_runtime.phase = phase;
  if (event->count_value >
      event->alarm_value + CONFIG_HEATER_MAX_ISR_LATENCY_US) {
    // 关断后只在未来的绝对周期边界恢复，避免将过去时间重新设为闹钟。
    heater_board_requests_off();
    heater_board_pwm_low();
    s_runtime.bootstrap_ready = false;
    s_runtime.started_mask = 0;
    s_runtime.healthy_mask = 0;
    s_runtime.faults |= HEATER_FAULT_TIMING;
    ++s_runtime.deadline_misses;
    s_runtime.phase = PHASE_ABORT;
    s_runtime.next_phase = PHASE_START;
    s_runtime.next_cycle_start_us =
        (event->count_value / HEATER_PERIOD_US + 1) * HEATER_PERIOD_US;
    s_runtime.alarm.alarm_count = s_runtime.next_cycle_start_us;
    notification = EVENT_ABORT;
  } else {
    switch (phase) {
      case PHASE_START: {
        s_runtime.cycle_start_us = s_runtime.next_cycle_start_us;
        ++s_runtime.sequence;
        s_runtime.started_mask = 0;
        s_runtime.ready_mask = 0;
        if ((s_runtime.run_status == HEATER_RUN_RUNNING) &&
            event->count_value >= s_runtime.lease_deadline_us) {
          runtime_stop_locked(HEATER_FAULT_HOST_TIMEOUT);
          s_runtime.host_timeout = true;
          memset(s_runtime.desired, 0, sizeof(s_runtime.desired));
        }
        s_runtime.frame_run_id =
            (s_runtime.run_status == HEATER_RUN_RUNNING) ? s_runtime.run_id : 0;
        s_runtime.frame_generation = s_runtime.generation;
        s_runtime.faults =
            (s_runtime.pwm_failed ? HEATER_FAULT_PWM : 0) |
            (s_runtime.host_timeout ? HEATER_FAULT_HOST_TIMEOUT : 0);
        const bool previous_done =
            s_runtime.healthy_sequence == s_runtime.sequence - 1;
        if (!previous_done) {
          s_runtime.faults |= HEATER_FAULT_TIMING;
          ++s_runtime.deadline_misses;
        }
        const unsigned result_slot = s_runtime.sequence % 4;
        const bool ready =
            s_runtime.results[result_slot].apply_sequence ==
                s_runtime.sequence &&
            s_runtime.results[result_slot].run_id == s_runtime.run_id &&
            s_runtime.results[result_slot].config_id == s_runtime.config_id &&
            s_runtime.results[result_slot].generation == s_runtime.generation;
        const bool controller_required =
            (s_runtime.mode == HEATER_MODE_RESISTANCE) &&
            (s_runtime.run_status == HEATER_RUN_RUNNING);
        if (controller_required &&
            s_runtime.sequence - s_runtime.start_sequence >=
                CONTROLLER_DELAY_CYCLES &&
            !ready) {
          runtime_stop_locked(HEATER_FAULT_CONTROLLER);
          ++s_runtime.controller_faults;
          s_runtime.faults |= HEATER_FAULT_CONTROLLER;
          heater_board_requests_off();
          heater_board_pwm_low();
        }
        s_runtime.applied_experiment = (heater_experiment_t){
            .run_id = s_runtime.frame_run_id,
            .config_id = s_runtime.config_id,
            .mode = (s_runtime.mode == HEATER_MODE_RESISTANCE),
            .algorithm = (s_runtime.mode == HEATER_MODE_RESISTANCE)
                             ? s_runtime.algorithm_id
                             : 0,
        };
        if ((s_runtime.mode == HEATER_MODE_RESISTANCE)) {
          memcpy(s_runtime.applied_experiment.target, s_runtime.target,
                 sizeof(s_runtime.target));
          if (ready) {
            s_runtime.applied_experiment.source_sequence =
                s_runtime.results[result_slot].output.source_sequence;
            s_runtime.applied_experiment.computation_us =
                s_runtime.results[result_slot].computation_us;
            s_runtime.applied_experiment.limited_mask =
                s_runtime.results[result_slot].output.limited_mask;
            memcpy(s_runtime.applied_experiment.requested,
                   s_runtime.results[result_slot].output.requested,
                   sizeof(s_runtime.applied_experiment.requested));
          }
        }
        bool heat = false;
        for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
          const bool allowed = previous_done && s_runtime.bootstrap_ready &&
                               !s_runtime.pwm_failed &&
                               (s_runtime.run_status == HEATER_RUN_RUNNING) &&
                               (s_runtime.healthy_mask & (1U << i));
          const uint8_t ticks =
              (s_runtime.mode == HEATER_MODE_RESISTANCE)
                  ? (ready ? s_runtime.results[result_slot].output.ticks[i] : 0)
                  : s_runtime.desired[i] * 2U;
          s_runtime.applied_experiment.pwm_ticks[i] = allowed ? ticks : 0;
          if (!allowed) s_runtime.applied_experiment.blocked_mask |= 1U << i;
          s_runtime.applied[i] = s_runtime.applied_experiment.pwm_ticks[i] / 2U;
          heat |= s_runtime.applied_experiment.pwm_ticks[i] != 0;
        }
        heater_board_measure_request(false);
        const bool pwm_ok = heater_board_pwm_apply_ticks(
            s_runtime.applied_experiment.pwm_ticks);
        if (!pwm_ok) {
          runtime_pwm_failure();
        } else {
          heater_board_heat_request(heat);
        }
        notification = EVENT_FRAME_START;
        break;
      }
      case PHASE_DISCHARGE:
        // IN=0 将两个电流端接地放电，不施加 48 V 加热功率。
        if (!heater_board_pwm_low()) {
          runtime_pwm_failure();
        } else if (!s_runtime.pwm_failed) {
          heater_board_heat_request(true);
        }
        s_runtime.bootstrap_ready = false;
        break;
      case PHASE_ISOLATE:
        heater_board_heat_request(false);
        break;
      case PHASE_MEASURE_ON:
        if (!s_runtime.pwm_failed) {
          heater_board_measure_request(true);
        }
        break;
      case PHASE_CONVERT:
        notification = EVENT_CONVERT;
        break;
      case PHASE_MEASURE_OFF:
        // 由定时器中断强制截止，不依赖 SPI、DRDY、控制任务或 USB 的完成情况。
        heater_board_measure_request(false);
        s_runtime.started_mask = 0;
        break;
      case PHASE_DONE:
        notification = EVENT_FRAME_DONE;
        break;
      case PHASE_PRECHARGE:
        // PhotoMOS 已关闭超过 800 µs；PWM 保持为低，仅给自举电容预充电。
        if (!s_runtime.pwm_failed && heater_board_pwm_low()) {
          heater_board_heat_request(true);
          s_runtime.bootstrap_ready = true;
        } else {
          runtime_pwm_failure();
        }
        break;
      default:
        break;
    }
    s_runtime.next_phase = phase + 1;
    if (s_runtime.next_phase == PHASE_COUNT) {
      s_runtime.next_phase = PHASE_START;
      s_runtime.next_cycle_start_us += HEATER_PERIOD_US;
    }
    s_runtime.alarm.alarm_count =
        s_runtime.next_cycle_start_us + s_phase_us[s_runtime.next_phase];
  }
  portEXIT_CRITICAL_ISR(&s_runtime.lock);
  uint64_t after;
  if (gptimer_get_raw_count(timer, &after) != ESP_OK) {
    portENTER_CRITICAL_ISR(&s_runtime.lock);
    heater_board_requests_off();
    s_runtime.pwm_failed = true;
    s_runtime.faults |= HEATER_FAULT_TIMING;
    s_runtime.phase = PHASE_ABORT;
    portEXIT_CRITICAL_ISR(&s_runtime.lock);
    gptimer_stop(timer);
    xTaskNotifyFromISR(s_runtime.task, EVENT_ABORT, eSetBits, &woken);
    return woken == pdTRUE;
  }
  if (after + ALARM_REARM_GUARD_US >= s_runtime.alarm.alarm_count) {
    // 回调内部的驱动操作也可能耗尽下一阶段裕量，重新检查并只安排未来闹钟。
    portENTER_CRITICAL_ISR(&s_runtime.lock);
    heater_board_requests_off();
    s_runtime.bootstrap_ready = false;
    s_runtime.started_mask = 0;
    s_runtime.healthy_mask = 0;
    s_runtime.faults |= HEATER_FAULT_TIMING;
    ++s_runtime.deadline_misses;
    s_runtime.phase = PHASE_ABORT;
    s_runtime.next_phase = PHASE_START;
    s_runtime.next_cycle_start_us =
        (after / HEATER_PERIOD_US + 1) * HEATER_PERIOD_US;
    if (after + ALARM_REARM_GUARD_US >= s_runtime.next_cycle_start_us) {
      s_runtime.next_cycle_start_us += HEATER_PERIOD_US;
    }
    s_runtime.alarm.alarm_count = s_runtime.next_cycle_start_us;
    portEXIT_CRITICAL_ISR(&s_runtime.lock);
    notification = EVENT_ABORT;
  }
  if (gptimer_set_alarm_action(timer, &s_runtime.alarm) != ESP_OK) {
    portENTER_CRITICAL_ISR(&s_runtime.lock);
    heater_board_requests_off();
    s_runtime.pwm_failed = true;
    s_runtime.faults |= HEATER_FAULT_TIMING;
    s_runtime.phase = PHASE_ABORT;
    portEXIT_CRITICAL_ISR(&s_runtime.lock);
    notification |= EVENT_ABORT;
  }
  if (notification) {
    xTaskNotifyFromISR(s_runtime.task, notification, eSetBits, &woken);
  }
  return woken == pdTRUE;
}
