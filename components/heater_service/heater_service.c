// 应用服务编排配置、运行和记录；运行真值始终由实时内核持有。
#include "heater_service.h"

#include <stdio.h>
#include <string.h>

#include "device_maintenance.h"
#include "heater_controller.h"
#include "heater_runtime.h"
#include "sdkconfig.h"
#include "telemetry_store.h"

// 固定校准值只在配置快照中复制，运行期间不写 Flash。
static heater_config_record_t configuration(uint32_t id, heater_mode_t mode,
                                            const controller_config_t* config,
                                            const uint8_t* duty) {
  heater_config_record_t record = {
      .config_id = id,
      .mode = mode,
      .reference_mohm = {CONFIG_HEATER_REFERENCE_1_MOHM,
                         CONFIG_HEATER_REFERENCE_2_MOHM,
                         CONFIG_HEATER_REFERENCE_3_MOHM,
                         CONFIG_HEATER_REFERENCE_4_MOHM},
      .offset_mohm = {CONFIG_HEATER_OFFSET_1_MOHM, CONFIG_HEATER_OFFSET_2_MOHM,
                      CONFIG_HEATER_OFFSET_3_MOHM,
                      CONFIG_HEATER_OFFSET_4_MOHM}};
  if (config)
    record.config = *config;
  else
    record.config.max_duty = HEATER_MAX_DUTY;
  if (duty)
    for (unsigned i = 0; i < 4; ++i) record.manual_ticks[i] = duty[i] * 2U;
  unsigned count;
  const controller_descriptor_t* registry = heater_controller_registry(&count);
  for (unsigned i = 0; i < count; ++i)
    if (registry[i].id == record.config.algorithm)
      snprintf(record.algorithm_version, sizeof(record.algorithm_version), "%s",
               registry[i].version);
  return record;
}
static uint32_t next_config_id(uint32_t id) {
  return id == UINT32_MAX ? 1 : id + 1;
}

esp_err_t heater_service_submit(const heater_command_t* command) {
  if (!command) return ESP_ERR_INVALID_ARG;
  if (command->kind == HEATER_COMMAND_STOP) {
    const esp_err_t result = heater_runtime_stop();
    telemetry_store_sync_stop();
    return result;
  }
  if (!device_maintenance_verified() || !device_operation_take(0))
    return ESP_ERR_INVALID_STATE;
  const uint32_t generation = heater_runtime_run_state().generation;
  esp_err_t result = ESP_OK;
  const bool start = command->kind == HEATER_COMMAND_START ||
                     command->kind == HEATER_COMMAND_RESISTANCE_START;
  if (start && heater_runtime_is_maintenance())
    result = heater_runtime_resume();
  const heater_controller_status_t status = heater_runtime_controller_status();
  if (result == ESP_OK && (command->kind == HEATER_COMMAND_SET ||
                           command->kind == HEATER_COMMAND_START)) {
    if (!heater_duties_valid(command->duty, HEATER_MAX_DUTY) ||
        (status.resistance_mode && heater_runtime_run_state().running))
      result = ESP_ERR_INVALID_ARG;
    else {
      const heater_config_record_t record =
          configuration(next_config_id(status.config_id), HEATER_MODE_MANUAL,
                        NULL, command->duty);
      result = telemetry_store_prepare_config(&record);
    }
  }
  if (result == ESP_OK && command->kind == HEATER_COMMAND_RESISTANCE_START) {
    const heater_config_record_t record = configuration(
        status.config_id, HEATER_MODE_RESISTANCE, &status.config, NULL);
    result = status.ready ? telemetry_store_prepare_config(&record)
                          : ESP_ERR_INVALID_STATE;
  }
  if (result == ESP_OK)
    result = heater_runtime_submit_in_generation(command, generation);
  device_operation_give();
  return result;
}
void heater_service_link_lost(void) { heater_runtime_stop(); }
esp_err_t heater_service_configure(const controller_config_t* config,
                                   uint32_t* config_id) {
  if (!device_maintenance_verified() || !device_operation_take(0))
    return ESP_ERR_INVALID_STATE;
  esp_err_t result = ESP_OK;
  const heater_controller_status_t previous =
      heater_runtime_controller_status();
  if (heater_runtime_run_state().running)
    result = ESP_ERR_INVALID_STATE;
  else if (!heater_controller_validate(config))
    result = ESP_ERR_INVALID_ARG;
  if (result == ESP_OK) {
    for (unsigned channel = 0; channel < 4; ++channel)
      if (config->target[channel] < CONFIG_HEATER_VALID_MIN_MOHM / 1000.0f ||
          config->target[channel] > CONFIG_HEATER_VALID_MAX_MOHM / 1000.0f)
        result = ESP_ERR_INVALID_ARG;
  }
  // 维护操作门闩已经取得；失败维护结束后可恢复安全采样再准备配置。
  // 此步骤不启动实验，不加热，也不允许在 Flash 事务期间恢复。
  if (result == ESP_OK && heater_runtime_is_maintenance())
    result = heater_runtime_resume();
  const uint32_t next = next_config_id(previous.config_id);
  if (result == ESP_OK) {
    // 先预留元数据，再提交配置；容量拒绝不能改变原算法配置。
    const heater_config_record_t record =
        configuration(next, HEATER_MODE_RESISTANCE, config, NULL);
    result = telemetry_store_prepare_config(&record);
  }
  if (result == ESP_OK) result = heater_runtime_configure(config);
  if (result == ESP_OK && config_id) *config_id = next;
  device_operation_give();
  return result;
}
heater_run_state_t heater_service_run_state(void) {
  return heater_runtime_run_state();
}
heater_controller_status_t heater_service_controller_status(void) {
  return heater_runtime_controller_status();
}
heater_timing_stats_t heater_service_timing_stats(void) {
  return heater_runtime_timing_stats();
}
bool heater_service_measurement(heater_telemetry_t* frame) {
  return heater_runtime_latest(frame);
}
bool heater_service_is_maintenance(void) {
  return heater_runtime_is_maintenance();
}

uint64_t heater_service_measurement_age_us(void) {
  return heater_runtime_measurement_age_us();
}
