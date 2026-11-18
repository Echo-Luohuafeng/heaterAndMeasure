// 算法工作任务的最小契约，不暴露 GPIO、PWM、SPI 或实时共享状态。
#pragma once
#include "heater_controller.h"
#include "heater_runtime.h"
esp_err_t runtime_worker_start(void);
bool runtime_worker_configure(const controller_config_t* config,
                              uint32_t config_id);
void runtime_worker_frame(const heater_telemetry_t* frame, uint32_t run_id,
                          uint32_t generation);
controller_config_t runtime_worker_config(void);
heater_config_state_t runtime_worker_state(void);
void runtime_controller_ready(uint32_t config_id, bool ready);
