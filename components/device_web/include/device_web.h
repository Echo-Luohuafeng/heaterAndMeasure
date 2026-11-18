#pragma once
#include "esp_err.h"

// 启动网页、HTTP/WebSocket 服务、每秒推送及启动版本自检任务。
// `control_init_result` 为控制初始化结果；失败时仍提供网页、配网与 OTA 恢复。
// 要求网络和历史缓存已初始化；返回 ESP_OK 或信号量、服务、任务创建错误。
esp_err_t device_web_start(esp_err_t control_init_result);
