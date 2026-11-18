// 保留已验证的时序；所有硬件操作属于实时执行组件。
#include "runtime_internal.h"
// 仅当 `sequence` 仍对应当前周期时合并 `fault`，避免迟到操作污染下一帧。
void runtime_add_fault(uint32_t sequence, uint32_t fault) {
  portENTER_CRITICAL(&s_runtime.lock);
  if (s_runtime.sequence == sequence) {
    s_runtime.faults |= fault;
  }
  portEXIT_CRITICAL(&s_runtime.lock);
}

// 关闭请求并锁存 PWM 故障，禁止后续加热直到重启。
// 调用方须已持有控制状态锁；函数位于 IRAM，供阶段中断调用。
void IRAM_ATTR runtime_pwm_failure(void) {
  heater_board_requests_off();
  s_runtime.pwm_failed = true;
  s_runtime.run_status = HEATER_RUN_IDLE;
  ++s_runtime.generation;
  if (s_runtime.on_run)
    s_runtime.on_run(false, s_runtime.run_id, s_runtime.generation);
  s_runtime.bootstrap_ready = false;
  s_runtime.faults |= HEATER_FAULT_PWM;
}

// 持 runtime lock 调用；回调只更新记录接收门，不接触 PSRAM。
bool IRAM_ATTR runtime_stop_locked(uint32_t fault) {
  s_runtime.run_status = HEATER_RUN_IDLE;
  ++s_runtime.generation;
  s_runtime.bootstrap_ready = false;
  s_runtime.started_mask = s_runtime.ready_mask = 0;
  memset(s_runtime.desired, 0, sizeof(s_runtime.desired));
  heater_board_requests_off();
  const bool low = heater_board_pwm_low();
  if (!low) runtime_pwm_failure();
  s_runtime.faults |= fault;
  if (s_runtime.on_run)
    s_runtime.on_run(false, s_runtime.run_id, s_runtime.generation);
  return low;
}
// 不读取 GPTimer，因此暂停或时钟异常时也能执行救援停止。
esp_err_t heater_runtime_stop(void) {
  portENTER_CRITICAL(&s_runtime.lock);
  if (!s_runtime.board_ready) {
    portEXIT_CRITICAL(&s_runtime.lock);
    return ESP_ERR_INVALID_STATE;
  }
  const bool success = runtime_stop_locked(HEATER_FAULT_STOPPED);
  s_runtime.host_timeout = false;
  portEXIT_CRITICAL(&s_runtime.lock);
  return success ? ESP_OK : ESP_FAIL;
}
void heater_runtime_set_run_callback(heater_runtime_run_callback_t callback) {
  portENTER_CRITICAL(&s_runtime.lock);
  s_runtime.on_run = callback;
  portEXIT_CRITICAL(&s_runtime.lock);
}
