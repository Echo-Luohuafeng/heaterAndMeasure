// 保留已验证的时序；所有硬件操作属于实时执行组件。
#include "runtime_internal.h"
// 在临界区内更新设定、运行 ID 和租约；STOP 直接关断，SET/PING 不启动空闲运行。
// 尚无控制任务时仅允许对已初始化板级输出执行 STOP。
esp_err_t heater_runtime_submit_in_generation(const heater_command_t* command,
                                              uint32_t generation) {
  if (command == NULL) {
    return ESP_ERR_INVALID_STATE;
  }
  if (command->kind == HEATER_COMMAND_STOP) return heater_runtime_stop();
  if (s_runtime.task == NULL) return ESP_ERR_INVALID_STATE;
  if (command->kind == HEATER_COMMAND_SET ||
      command->kind == HEATER_COMMAND_START) {
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
      if (command->duty[i] > HEATER_MAX_DUTY) {
        return ESP_ERR_INVALID_ARG;
      }
    }
  } else if (command->kind != HEATER_COMMAND_STOP &&
             command->kind != HEATER_COMMAND_PING &&
             command->kind != HEATER_COMMAND_RESISTANCE_START) {
    return ESP_ERR_INVALID_ARG;
  }
  uint64_t now;
  if (gptimer_get_raw_count(s_runtime.timer, &now) != ESP_OK) return ESP_FAIL;
  portENTER_CRITICAL(&s_runtime.lock);
  // STOP 可越过应用门闩，旧 START 即使随后恢复调度也不能重新加热。
  if (generation != s_runtime.generation) {
    portEXIT_CRITICAL(&s_runtime.lock);
    return ESP_ERR_INVALID_STATE;
  }
  if ((s_runtime.device_state == HEATER_DEVICE_PAUSING ||
       s_runtime.device_state == HEATER_DEVICE_PAUSED) &&
      command->kind != HEATER_COMMAND_STOP) {
    portEXIT_CRITICAL(&s_runtime.lock);
    return ESP_ERR_INVALID_STATE;
  }
  if ((command->kind == HEATER_COMMAND_RESISTANCE_START ||
       (command->kind == HEATER_COMMAND_START &&
        (s_runtime.mode == HEATER_MODE_RESISTANCE))) &&
      (s_runtime.run_status == HEATER_RUN_RUNNING)) {
    portEXIT_CRITICAL(&s_runtime.lock);
    return ESP_ERR_INVALID_STATE;
  }
  if (command->kind == HEATER_COMMAND_SET &&
      (s_runtime.mode == HEATER_MODE_RESISTANCE) &&
      (s_runtime.run_status == HEATER_RUN_RUNNING)) {
    portEXIT_CRITICAL(&s_runtime.lock);
    return ESP_ERR_INVALID_STATE;
  }
  if (command->kind == HEATER_COMMAND_RESISTANCE_START &&
      (!(s_runtime.mode == HEATER_MODE_RESISTANCE) ||
       s_runtime.ready_config_id != s_runtime.config_id ||
       (command->config_id && command->config_id != s_runtime.config_id))) {
    portEXIT_CRITICAL(&s_runtime.lock);
    return ESP_ERR_INVALID_STATE;
  }
  if (command->kind == HEATER_COMMAND_SET ||
      command->kind == HEATER_COMMAND_START ||
      command->kind == HEATER_COMMAND_RESISTANCE_START) {
    if (command->kind == HEATER_COMMAND_SET ||
        command->kind == HEATER_COMMAND_START) {
      s_runtime.mode = HEATER_MODE_MANUAL;
      if (++s_runtime.config_id == 0) ++s_runtime.config_id;
    }
    memcpy(s_runtime.desired, command->duty, sizeof(s_runtime.desired));
    if (command->kind == HEATER_COMMAND_START ||
        command->kind == HEATER_COMMAND_RESISTANCE_START) {
      if (command->kind == HEATER_COMMAND_START) {
        s_runtime.mode = HEATER_MODE_MANUAL;
      }
      s_runtime.start_sequence = s_runtime.sequence + 1;
      memset(s_runtime.results, 0, sizeof(s_runtime.results));
      if (++s_runtime.run_id == 0) ++s_runtime.run_id;
      ++s_runtime.generation;
      s_runtime.run_status = HEATER_RUN_RUNNING;
      if (s_runtime.on_run)
        s_runtime.on_run(true, s_runtime.run_id, s_runtime.generation);
    }
    if ((s_runtime.run_status == HEATER_RUN_RUNNING)) {
      s_runtime.host_timeout = false;
      s_runtime.lease_deadline_us =
          now + (uint64_t)CONFIG_HEATER_CONTROL_LEASE_MS * 1000;
      ++s_runtime.lease_refreshes;
    }
  } else if ((s_runtime.run_status == HEATER_RUN_RUNNING)) {
    s_runtime.lease_deadline_us =
        now + (uint64_t)CONFIG_HEATER_CONTROL_LEASE_MS * 1000;
    ++s_runtime.lease_refreshes;
  }
  portEXIT_CRITICAL(&s_runtime.lock);
  return ESP_OK;
}

// 在控制锁内同时读取运行标志和 ID，使缓存任务能一致地筛选入队样本。
heater_run_state_t heater_runtime_run_state(void) {
  portENTER_CRITICAL(&s_runtime.lock);
  const heater_run_state_t state = {
      .running = (s_runtime.run_status == HEATER_RUN_RUNNING) &&
                 !(s_runtime.device_state == HEATER_DEVICE_PAUSING ||
                   s_runtime.device_state == HEATER_DEVICE_PAUSED),
      .id = s_runtime.run_id,
      .generation = s_runtime.generation};
  portEXIT_CRITICAL(&s_runtime.lock);
  return state;
}

// 在控制锁内读取任务已确认的暂停标志，区别于仅发起维护请求。
bool heater_runtime_is_paused(void) {
  portENTER_CRITICAL(&s_runtime.lock);
  const bool paused = (s_runtime.device_state == HEATER_DEVICE_PAUSED);
  portEXIT_CRITICAL(&s_runtime.lock);
  return paused;
}

// 在控制锁内复制启动最大偏移及两类截止失败计数。
heater_timing_stats_t heater_runtime_timing_stats(void) {
  portENTER_CRITICAL(&s_runtime.lock);
  const heater_timing_stats_t result = s_runtime.timing;
  portEXIT_CRITICAL(&s_runtime.lock);
  return result;
}

// 在控制锁内读取维护请求标志，该标志可先于实际暂停确认生效。
bool heater_runtime_is_maintenance(void) {
  portENTER_CRITICAL(&s_runtime.lock);
  const bool paused = (s_runtime.device_state == HEATER_DEVICE_PAUSING ||
                       s_runtime.device_state == HEATER_DEVICE_PAUSED);
  portEXIT_CRITICAL(&s_runtime.lock);
  return paused;
}

// 用生命周期互斥锁串行化暂停/恢复，先关断并清空运行，再等待任务应答。
// 无控制任务时直接核验板级关断；超时保持维护状态，禁止写 Flash。
esp_err_t heater_runtime_pause(TickType_t timeout) {
  if (s_runtime.lifecycle == NULL) return ESP_ERR_INVALID_STATE;
  if (xSemaphoreTake(s_runtime.lifecycle, timeout) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  if (s_runtime.task == NULL) {
    // 初始化结束但未启动控制任务，例如 ADC 寄存器验证失败。
    // 此时没有运行中的 SPI 事务或周期定时器，可直接核验板级关断。
    portENTER_CRITICAL(&s_runtime.lock);
    const bool ready = s_runtime.board_ready;
    if (ready) {
      runtime_stop_locked(0);
      s_runtime.device_state = HEATER_DEVICE_PAUSING;
      heater_board_requests_off();
      s_runtime.device_state =
          heater_board_pwm_low() ? HEATER_DEVICE_PAUSED : HEATER_DEVICE_PAUSING;
    }
    const bool safe = ready && (s_runtime.device_state == HEATER_DEVICE_PAUSED);
    portEXIT_CRITICAL(&s_runtime.lock);
    xSemaphoreGive(s_runtime.lifecycle);
    return safe ? ESP_OK : ESP_ERR_INVALID_STATE;
  }
  portENTER_CRITICAL(&s_runtime.lock);
  const bool paused = (s_runtime.device_state == HEATER_DEVICE_PAUSED);
  const bool low = runtime_stop_locked(0);
  if (!paused) s_runtime.device_state = HEATER_DEVICE_PAUSING;
  s_runtime.bootstrap_ready = false;
  s_runtime.started_mask = s_runtime.ready_mask = s_runtime.healthy_mask = 0;
  memset(s_runtime.desired, 0, sizeof(s_runtime.desired));
  memset(s_runtime.applied, 0, sizeof(s_runtime.applied));
  heater_board_requests_off();
  heater_board_pwm_low();
  portEXIT_CRITICAL(&s_runtime.lock);
  esp_err_t result = low ? ESP_OK : ESP_FAIL;
  if (!paused) {
    xSemaphoreTake(s_runtime.pause_ack, 0);  // 丢弃之前失败流程遗留的应答
    xTaskNotify(s_runtime.task, EVENT_PAUSE, eSetBits);
    if (xSemaphoreTake(s_runtime.pause_ack, timeout) != pdTRUE ||
        !(s_runtime.device_state == HEATER_DEVICE_PAUSED)) {
      result = ESP_ERR_TIMEOUT;
    }
  }
  xSemaphoreGive(s_runtime.lifecycle);
  return result;
}

// 串行化恢复操作，将实际暂停时长补入 GPTimer 并安排未来的周期边界。
// 启动失败重新锁定维护状态；成功后仍为空闲，需 START 创建新的显式运行。
esp_err_t heater_runtime_resume(void) {
  if (s_runtime.lifecycle == NULL || s_runtime.task == NULL ||
      s_runtime.timer == NULL)
    return ESP_ERR_INVALID_STATE;
  if (xSemaphoreTake(s_runtime.lifecycle, pdMS_TO_TICKS(1000)) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  esp_err_t result = ESP_ERR_INVALID_STATE;
  if ((s_runtime.device_state == HEATER_DEVICE_PAUSED)) {
    uint64_t now;
    result = gptimer_get_raw_count(s_runtime.timer, &now);
    if (result == ESP_OK) {
      now += (uint64_t)(esp_timer_get_time() - s_runtime.pause_start_us);
      result = gptimer_set_raw_count(s_runtime.timer, now);
    }
    if (result == ESP_OK) {
      s_runtime.next_cycle_start_us =
          (now / HEATER_PERIOD_US + 2) * HEATER_PERIOD_US;
      s_runtime.next_phase = PHASE_START;
      s_runtime.healthy_sequence = s_runtime.sequence;
      s_runtime.alarm.alarm_count = s_runtime.next_cycle_start_us;
      result = gptimer_set_alarm_action(s_runtime.timer, &s_runtime.alarm);
    }
    if (result == ESP_OK) {
      portENTER_CRITICAL(&s_runtime.lock);
      s_runtime.device_state = HEATER_DEVICE_READY;
      portEXIT_CRITICAL(&s_runtime.lock);
      result = gptimer_start(s_runtime.timer);
      if (result != ESP_OK) {
        portENTER_CRITICAL(&s_runtime.lock);
        s_runtime.device_state = HEATER_DEVICE_PAUSED;
        heater_board_requests_off();
        portEXIT_CRITICAL(&s_runtime.lock);
      }
    }
  }
  xSemaphoreGive(s_runtime.lifecycle);
  return result;
}

// 无排队的内核调用仍取得当前代次；外部用例必须携带开始编排时的代次。
esp_err_t heater_runtime_submit(const heater_command_t* command) {
  return heater_runtime_submit_in_generation(
      command, heater_runtime_run_state().generation);
}

esp_err_t heater_runtime_configure(const controller_config_t* config) {
  if (!heater_controller_validate(config)) return ESP_ERR_INVALID_ARG;
  for (unsigned i = 0; i < 4; ++i)
    if (config->target[i] < CONFIG_HEATER_VALID_MIN_MOHM / 1000.0f ||
        config->target[i] > CONFIG_HEATER_VALID_MAX_MOHM / 1000.0f)
      return ESP_ERR_INVALID_ARG;
  portENTER_CRITICAL(&s_runtime.lock);
  if (!s_runtime.task || s_runtime.run_status == HEATER_RUN_RUNNING ||
      s_runtime.device_state != HEATER_DEVICE_READY) {
    portEXIT_CRITICAL(&s_runtime.lock);
    return ESP_ERR_INVALID_STATE;
  }
  uint32_t next = s_runtime.config_id + 1;
  if (!next) ++next;
  // 内核锁使 ready 回调晚于配置身份发布；发送失败不修改当前配置。
  if (!runtime_worker_configure(config, next)) {
    portEXIT_CRITICAL(&s_runtime.lock);
    return ESP_ERR_INVALID_STATE;
  }
  s_runtime.config_id = next;
  s_runtime.mode = HEATER_MODE_RESISTANCE;
  s_runtime.ready_config_id = 0;
  s_runtime.algorithm_id = (uint8_t)config->algorithm;
  memcpy(s_runtime.target, config->target, sizeof(s_runtime.target));
  memset(s_runtime.desired, 0, sizeof(s_runtime.desired));
  portEXIT_CRITICAL(&s_runtime.lock);
  return ESP_OK;
}
heater_controller_status_t heater_runtime_controller_status(void) {
  uint64_t now = 0;
  if (s_runtime.timer) gptimer_get_raw_count(s_runtime.timer, &now);
  portENTER_CRITICAL(&s_runtime.lock);
  heater_controller_status_t status = {
      .resistance_mode = s_runtime.mode == HEATER_MODE_RESISTANCE,
      .config_id = s_runtime.config_id,
      .ready = s_runtime.mode == HEATER_MODE_RESISTANCE &&
               s_runtime.ready_config_id == s_runtime.config_id,
      .worker_stack_free_bytes = s_runtime.worker_stack_free_bytes,
      .controller_faults = s_runtime.controller_faults,
      .lease_refreshes = s_runtime.lease_refreshes,
      .lease_remaining_ms = s_runtime.run_status == HEATER_RUN_RUNNING &&
                                    s_runtime.lease_deadline_us > now
                                ? (s_runtime.lease_deadline_us - now) / 1000
                                : 0};
  portEXIT_CRITICAL(&s_runtime.lock);
  status.config = status.resistance_mode
                      ? runtime_worker_config()
                      : (controller_config_t){.max_duty = HEATER_MAX_DUTY};
  status.state =
      status.resistance_mode ? runtime_worker_state() : HEATER_CONFIG_EMPTY;
  return status;
}
// 算法结果只写值与通知，任何硬件关断均由实时组件负责。
void heater_runtime_controller_result(uint32_t run_id, uint32_t config_id,
                                      uint32_t generation,
                                      uint32_t computation_us,
                                      const controller_output_t* output) {
  bool fault = false;
  const uint32_t stack = uxTaskGetStackHighWaterMark(NULL);
  portENTER_CRITICAL(&s_runtime.lock);
  s_runtime.worker_stack_free_bytes = stack;
  if (s_runtime.run_status == HEATER_RUN_RUNNING &&
      s_runtime.mode == HEATER_MODE_RESISTANCE &&
      s_runtime.device_state == HEATER_DEVICE_READY &&
      s_runtime.run_id == run_id && s_runtime.config_id == config_id &&
      s_runtime.generation == generation) {
    const uint32_t apply =
        output ? output->source_sequence + CONTROLLER_DELAY_CYCLES : 0;
    if (!output || !output->valid) {
      s_runtime.controller_fault_run_id = run_id;
      s_runtime.controller_fault_generation = generation;
      fault = true;
    } else if ((int32_t)(apply - s_runtime.sequence) > 0) {
      const unsigned slot = apply % 4;
      s_runtime.results[slot].run_id = run_id;
      s_runtime.results[slot].config_id = config_id;
      s_runtime.results[slot].generation = generation;
      s_runtime.results[slot].apply_sequence = apply;
      s_runtime.results[slot].computation_us = computation_us;
      s_runtime.results[slot].output = *output;
    }
  }
  portEXIT_CRITICAL(&s_runtime.lock);
  if (fault) xTaskNotify(s_runtime.task, EVENT_CONTROLLER_FAULT, eSetBits);
}

// 配置准备回调只修改身份匹配的状态，旧配置不能解除新配置的等待。
void runtime_controller_ready(uint32_t config_id, bool ready) {
  portENTER_CRITICAL(&s_runtime.lock);
  if (s_runtime.config_id == config_id &&
      s_runtime.mode == HEATER_MODE_RESISTANCE)
    s_runtime.ready_config_id = ready ? config_id : 0;
  portEXIT_CRITICAL(&s_runtime.lock);
}
