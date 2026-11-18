#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "heater_protocol.h"

// 引脚依据 J901/J902 与标准 ESP32-S3-DevKitC-1 排针布局推定。
// GPIO48 保留给板载 RGB LED，兼容板需核对实物接线。
#define HEATER_HEAT_REQ_GPIO 15
#define HEATER_MEAS_REQ_GPIO 16
#define HEATER_SPI_MOSI_GPIO 11
#define HEATER_SPI_SCK_GPIO 12
#define HEATER_SPI_MISO_GPIO 13
#define HEATER_PWM_FREQUENCY_HZ 10000U

extern const int heater_pwm_gpio[HEATER_CHANNEL_COUNT];
extern const int heater_adc_cs_gpio[HEATER_CHANNEL_COUNT];
extern const int heater_adc_drdy_gpio[HEATER_CHANNEL_COUNT];

// 初始化请求 GPIO 与四路 10 kHz MCPWM，并保持输出为低。
// 应在控制任务启动前调用一次；返回 ESP_OK 或外设初始化错误。
esp_err_t heater_board_init(void);
// 强制四路 PWM 持续为低，全部成功时返回 true。
// 要求板级 PWM 已初始化；可在 ISR 中调用，不阻塞、不分配内存。
bool heater_board_pwm_low(void);
// 同步应用非空的四路 `duty` 数组，各值为加热窗口内的百分比（0～95）。
// 要求 PWM 已初始化且四路已被强制拉低；零占空比保持为低。
// 可在 ISR 中调用；失败返回 false，调用方必须关闭请求并锁定加热。
bool heater_board_pwm_apply(const uint8_t duty[HEATER_CHANNEL_COUNT]);
// 拉低加热和测量请求，断开两个请求控制的路径。
// 可在 ISR 中调用；PWM 强制拉低需另行调用 heater_board_pwm_low()。
void heater_board_requests_off(void);
// 将加热请求设为 `enabled`，可在 ISR 中调用。
// 调用方负责与测量请求互锁；PWM 为低时可用于放电或自举预充电。
void heater_board_heat_request(bool enabled);
// 将测量请求设为 `enabled`，可在 ISR 中调用。
// 调用方必须先关闭加热请求并预留互锁切换时间。
void heater_board_measure_request(bool enabled);

// 0.5 个百分点的 PWM 比较器计数；与整数接口使用相同的安全上限。
bool heater_board_pwm_apply_ticks(const uint8_t ticks[4]);
