#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// connected 表示 USB 已枚举且未挂起；不等同于浏览器或控制租约在线。
typedef struct {
  bool connected;
  char address[16];
  uint32_t rx_drops;
  uint32_t tx_drops;
  uint32_t usb_stack_free_bytes;
  uint32_t network_stack_free_bytes;
} network_status_t;

// 初始化 USB Ethernet/lwIP 接口；凭据由维护服务在启动前载入。
esp_err_t device_usb_init(void);
// 启动 DHCP 与 TinyUSB CDC-NCM/ACM 复合设备，USB 相关任务均位于 Core 0。
esp_err_t device_usb_start(void);
void device_usb_status(network_status_t* status);
// 救援串口打开后在任务上下文重发地址及持久口令，便于首次连接。
void device_usb_log_connection(void);

// 同一复合设备的 ACM 日志及 STOP/PING 救援入口。
esp_err_t device_usb_console_start(void);
