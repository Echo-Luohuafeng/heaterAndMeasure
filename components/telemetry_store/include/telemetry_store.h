#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "telemetry_ring.h"
#include "esp_err.h"

esp_err_t telemetry_store_init(void);
esp_err_t telemetry_store_start(QueueHandle_t samples);
bool telemetry_store_uses_psram(void);
telemetry_stats_t telemetry_store_stats(void);
bool telemetry_store_latest(heater_telemetry_t *frame);
size_t telemetry_store_read(uint64_t from, telemetry_record_t *output, size_t limit,
                            uint64_t *next, uint64_t *lost, telemetry_stats_t *stats);
