#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

esp_err_t host_link_start(QueueHandle_t outbound);
