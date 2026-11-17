#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "heater_protocol.h"

esp_err_t heater_control_init(QueueHandle_t outbound);
esp_err_t heater_control_start(void);
// SET is latched at the next frame boundary; STOP removes heat immediately.
// SET persists until replaced, stopped or reset; measurement/phase guards still apply.
// PING checks communication without changing the heating state.
esp_err_t heater_control_submit(const heater_command_t *command);
