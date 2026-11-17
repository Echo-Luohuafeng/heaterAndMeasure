#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "heater_protocol.h"

esp_err_t heater_control_init(QueueHandle_t outbound);
esp_err_t heater_control_start(void);
// SET is latched at the next frame boundary; STOP removes heat immediately.
// SET/PING renew the control lease; expiry cancels heating but keeps measuring.
esp_err_t heater_control_submit(const heater_command_t *command);
// A successful pause acknowledges that the timer and all ADC transfers stopped.
// Timeout leaves outputs inhibited. Flash writes must wait for ESP_OK.
esp_err_t heater_control_pause(TickType_t timeout);
esp_err_t heater_control_resume(void);
bool heater_control_is_paused(void);
bool heater_control_is_maintenance(void);
typedef struct {
    uint32_t adc_start_max_offset_us;
    uint32_t adc_start_misses;
    uint32_t adc_read_misses;
} heater_timing_stats_t;
heater_timing_stats_t heater_control_timing_stats(void);
