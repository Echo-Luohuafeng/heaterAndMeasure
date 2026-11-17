#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// Only initialization and the control task access the shared SPI bus.
esp_err_t ads1220_init(void);
esp_err_t ads1220_start(size_t channel);
esp_err_t ads1220_read(size_t channel, int32_t *code);
