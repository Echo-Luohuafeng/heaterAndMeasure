// 设备口令与网络传输分离，运行期间口令不可变。
#pragma once
#include <stdbool.h>

#include "esp_err.h"
esp_err_t device_credentials_init(void);
bool device_credentials_authorized(const char* token);
// 救援串口打开后显示现有口令与地址，不触发 NVS 写入。
void device_credentials_log(const char* address);
