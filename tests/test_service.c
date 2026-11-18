// 独立链接应用服务，验证 STOP 越过门闩及配置容量的事务顺序。
#include <assert.h>
#include <stdio.h>

#include "heater_runtime.h"
#include "heater_service.h"
#include "telemetry_store.h"

static bool s_verified = true, s_available = true, s_interrupt_start;
static bool s_capacity_fails, s_configured, s_barrier, s_maintenance;
static heater_run_state_t s_run;
static heater_controller_status_t s_controller = {.config_id = 1};
bool device_maintenance_verified(void) { return s_verified; }
bool device_operation_take(TickType_t timeout) {
  (void)timeout;
  if (!s_available) return false;
  s_available = false;
  return true;
}
void device_operation_give(void) { s_available = true; }
esp_err_t heater_runtime_stop(void) {
  s_run.running = false;
  ++s_run.generation;
  return ESP_OK;
}
void telemetry_store_sync_stop(void) {
  assert(!s_run.running);
  s_barrier = true;
}
heater_run_state_t heater_runtime_run_state(void) { return s_run; }
heater_controller_status_t heater_runtime_controller_status(void) {
  return s_controller;
}
bool heater_runtime_is_maintenance(void) { return s_maintenance; }
esp_err_t heater_runtime_resume(void) {
  s_maintenance = false;
  return ESP_OK;
}
esp_err_t telemetry_store_prepare_config(const heater_config_record_t* config) {
  assert(config->config_id == s_controller.config_id + 1);
  assert(config->reference_mohm[0] == 1000000);
  if (s_interrupt_start) heater_service_link_lost();
  return s_capacity_fails ? ESP_ERR_NO_MEM : ESP_OK;
}
esp_err_t heater_runtime_submit_in_generation(const heater_command_t* command,
                                              uint32_t generation) {
  (void)command;
  if (generation != s_run.generation) return ESP_ERR_INVALID_STATE;
  s_run.running = true;
  return ESP_OK;
}
esp_err_t heater_runtime_configure(const controller_config_t* config) {
  assert(config);
  s_configured = true;
  ++s_controller.config_id;
  return ESP_OK;
}
heater_timing_stats_t heater_runtime_timing_stats(void) {
  return (heater_timing_stats_t){0};
}
bool heater_runtime_latest(heater_telemetry_t* frame) {
  (void)frame;
  return false;
}
uint64_t heater_runtime_measurement_age_us(void) { return UINT64_MAX; }
int main(void) {
  const heater_command_t start = {.kind = HEATER_COMMAND_START,
                                  .duty = {1, 1, 1, 1}};
  s_interrupt_start = true;
  assert(heater_service_submit(&start) == ESP_ERR_INVALID_STATE);
  assert(!s_run.running && s_available);
  const heater_command_t stop = {.kind = HEATER_COMMAND_STOP};
  s_available = false;
  assert(heater_service_submit(&stop) == ESP_OK && s_barrier && !s_available);
  s_available = true;
  controller_config_t config = {.algorithm = CONTROLLER_PID,
                                .max_duty = HEATER_MAX_DUTY,
                                .tolerance_ohm = .05f};
  config.parameters[12] = .1f;
  for (unsigned i = 0; i < 4; ++i) config.target[i] = 110;
  s_interrupt_start = false;
  s_capacity_fails = true;
  assert(heater_service_configure(&config, NULL) == ESP_ERR_NO_MEM);
  assert(!s_configured && s_controller.config_id == 1);
  s_capacity_fails = false;
  uint32_t config_id;
  assert(heater_service_configure(&config, &config_id) == ESP_OK);
  assert(s_configured && config_id == 2);
  s_maintenance = true;
  assert(heater_service_configure(&config, &config_id) == ESP_OK);
  assert(!s_maintenance && !s_run.running && config_id == 3);
  puts("应用服务停止竞争、越过门闩及配置容量事务测试通过");
  return 0;
}
