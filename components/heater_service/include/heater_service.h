// 所有外部适配器统一调用应用用例，不直接修改实时内核状态。
#pragma once
#include "esp_err.h"
#include "heater_domain.h"

// STOP 快速关断后形成记录屏障；普通命令由应用操作门闩串行化。
esp_err_t heater_service_submit(const heater_command_t* command);
// USB 断开回调只请求快速停止，不阻塞等待历史锁。
void heater_service_link_lost(void);
// 配置已接受后返回 config_id；调用方等待同一 ID ready 后 RSTART。
esp_err_t heater_service_configure(const controller_config_t* config,
                                   uint32_t* config_id);
heater_run_state_t heater_service_run_state(void);
heater_controller_status_t heater_service_controller_status(void);
heater_timing_stats_t heater_service_timing_stats(void);
// 查询当前安全样本，与实验历史的 latest 明确分开。
bool heater_service_measurement(heater_telemetry_t* frame);
bool heater_service_is_maintenance(void);

// 当前测量距发布时刻的时间；未有样本时返回 UINT64_MAX。
uint64_t heater_service_measurement_age_us(void);
