#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 初始化四路 ADS1220，配置并读回验证寄存器，保留 IDAC 激励。
// 必须在控制任务启动前调用；SPI 总线由本模块长期独占，后续访问不得并发。
// 返回 ESP_OK 表示成功，否则返回 GPIO/SPI 初始化或寄存器校验错误。
esp_err_t ads1220_init(void);
// 启动 `channel` 指定通道的一次转换，通道索引为 0～3。
// 仅供初始化完成后的控制任务调用；返回 ESP_OK 或参数/SPI 错误。
esp_err_t ads1220_start(size_t channel);
// 读取 `channel` 通道的 24 位有符号码到非空的 `code`。
// 调用前应确认 DRDY 就绪并仍处于测量窗口；失败时不改写 `code`。
// 仅供控制任务调用；返回 ESP_OK 或参数/SPI 错误。
esp_err_t ads1220_read(size_t channel, int32_t* code);
