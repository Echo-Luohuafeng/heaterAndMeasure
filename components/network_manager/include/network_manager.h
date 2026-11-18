#pragma once

#include <stdbool.h>
#include "esp_err.h"

// 保存网络状态的调用方副本；address 为 IPv4 文本，rssi 单位为 dBm。
// 未获得地址或信号强度时字段为零值，ap_ssid 始终给出设备恢复热点名称。
typedef struct {
    bool connected;
    bool ap_active;
    char address[16];
    char ap_ssid[32];
    int rssi;
} network_status_t;

// 初始化 NVS、设备口令、网络接口和 Wi-Fi 配置，不启动无线连接。
// 会持久化首次生成的口令并进行驱动初始化，必须在采样启动前调用。
// 返回 ESP_OK 或 NVS/内存错误；底层 ESP_ERROR_CHECK 失败会终止启动。
esp_err_t network_manager_init(void);
// 启动 Wi-Fi；已有路由器配置时创建 Core 0 重连任务，否则提供恢复热点。
// 要求网络初始化成功；返回 ESP_OK 或 Wi-Fi 启动、任务创建错误。
// 发射功率及省电配置失败由 ESP_ERROR_CHECK 终止启动。
esp_err_t network_manager_start(void);
// 将连接状态、恢复热点名称、IPv4 地址与 RSSI 写入非空的 `status`。
// 要求网络初始化完成；无有效地址或 RSSI 时相应字段保持零值。
void network_manager_status(network_status_t *status);
// 校验以 NUL 结尾的 `token` 是否匹配设备的 32 位口令。
// NULL 或长度不符返回 false；合法长度逐字节比较后返回匹配结果。
bool network_manager_authorized(const char *token);
// 将 NUL 结尾的非空 `ssid`、`password` 作为一个 NVS 配置块保存。
// SSID 长度为 1～32 字节；密码允许空串、8～63 字节或 64 位十六进制 PSK。
// 调用方必须先获得控制暂停确认；返回 ESP_OK 或状态、参数、NVS 写入错误。
// 保存后需重启才应用新配置。
esp_err_t network_manager_save(const char *ssid, const char *password);
