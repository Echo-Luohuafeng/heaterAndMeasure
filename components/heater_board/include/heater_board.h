#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "heater_protocol.h"

// Reference J901/J902, assuming the standard ESP32-S3-DevKitC-1 header layout.
// GPIO48 is reserved for the user's onboard RGB LED.
#define HEATER_HEAT_REQ_GPIO 15
#define HEATER_MEAS_REQ_GPIO 16
#define HEATER_SPI_MOSI_GPIO 11
#define HEATER_SPI_SCK_GPIO 12
#define HEATER_SPI_MISO_GPIO 13
#define HEATER_PWM_FREQUENCY_HZ 10000U

extern const int heater_pwm_gpio[HEATER_CHANNEL_COUNT];
extern const int heater_adc_cs_gpio[HEATER_CHANNEL_COUNT];
extern const int heater_adc_drdy_gpio[HEATER_CHANNEL_COUNT];

// Establish low GPIO outputs before configuring any other peripheral.
esp_err_t heater_board_init(void);
// These operations are bounded, allocate nothing, and can run in an ISR.
bool heater_board_pwm_low(void);
bool heater_board_pwm_apply(const uint8_t duty[HEATER_CHANNEL_COUNT]);
void heater_board_requests_off(void);
void heater_board_heat_request(bool enabled);
void heater_board_measure_request(bool enabled);
