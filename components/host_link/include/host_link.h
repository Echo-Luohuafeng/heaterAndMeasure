#pragma once

#include "esp_err.h"
// 初始化 TinyUSB CDC-ACM 日志串口并创建 Core 0 的救援接收任务。
// 仅处理 STOP/PING，不持续发送采样；返回 ESP_OK 或状态、驱动、内存错误。
esp_err_t host_link_start(void);
