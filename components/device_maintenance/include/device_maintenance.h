// 应用操作与 Flash 事务分别串行化；STOP 始终绕过操作门闩。
#pragma once
#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// 采样启动前创建门闩与凭据；初始化失败由启动入口关闭输出。
esp_err_t device_maintenance_init(void);
// 二值门闩允许 HTTP 将操作所有权转交给异步 OTA 工作任务。
bool device_operation_take(TickType_t timeout);
void device_operation_give(void);
// 先持有应用门闩；Flash 事务由同一任务取得与释放，确认暂停后才允许擦写。
esp_err_t device_flash_begin(TickType_t timeout);
esp_err_t device_flash_end(bool resume_sampling);
// 启动验证属于设备生命周期，独立于网页是否存在。
esp_err_t device_maintenance_verify_start(esp_err_t control_init_result);
bool device_maintenance_verified(void);
