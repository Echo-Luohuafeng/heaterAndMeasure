#include <stdio.h>
#include <string.h>

#include "device_usb.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heater_protocol.h"
#include "heater_service.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_console.h"

static bool s_started;
static TaskHandle_t s_rescue_task;

// 与日志共用 VFS 输出锁；TinyUSB CDC 输出不等待主机读取。
static void reply(const char* text) { fputs(text, stdout); }

// 回调只通知任务，口令日志及命令处理都在 usb_rescue 中执行。
static void line_state_changed(int itf, cdcacm_event_t* event) {
  (void)itf;
  if (event->line_state_changed_data.dtr && s_rescue_task)
    xTaskNotifyGive(s_rescue_task);
}

// 阻塞接收 USB 字节，按回车/换行分帧；超长行丢弃到行尾，仅接受 STOP/PING。
// STOP 后等待缓存写入屏障再确认；`arg` 未使用。
// usb_rescue 位于 Core 0，优先级 4，栈为 4096 字节。
static void receive_task(void* arg) {
  (void)arg;
  char line[HEATER_COMMAND_MAX_LENGTH];
  size_t used = 0;
  bool discard = false;
  for (;;) {
    uint8_t input[64];
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100))) {
      reply("READY,USB_NCM,50HZ,USB_STOP_ONLY\n");
      device_usb_log_connection();
    }
    size_t count = 0;
    tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, input, sizeof(input), &count);
    for (size_t i = 0; i < count; ++i) {
      const char c = (char)input[i];
      if (c == '\r' || c == '\n') {
        heater_command_t command;
        if (discard)
          reply("ERR,LINE_TOO_LONG\n");
        else if (used) {
          if (!heater_command_parse(line, used, &command))
            reply("ERR,COMMAND\n");
          else if (command.kind == HEATER_COMMAND_SET ||
                   command.kind == HEATER_COMMAND_START ||
                   command.kind == HEATER_COMMAND_RESISTANCE_START)
            reply("ERR,USE_HTTP\n");
          else if (heater_service_submit(&command) != ESP_OK)
            reply("ERR,MAINTENANCE\n");
          else {
            reply(command.kind == HEATER_COMMAND_STOP ? "ACK,STOP\n"
                                                      : "ACK,PING\n");
          }
        }
        used = 0;
        discard = false;
      } else if (!discard) {
        if (used == sizeof(line))
          discard = true;
        else
          line[used++] = c;
      }
    }
  }
}

// 网络模块先安装 TinyUSB；这里初始化同一复合设备的 ACM 日志/救援串口。
esp_err_t device_usb_console_start(void) {
  if (s_started) return ESP_ERR_INVALID_STATE;
  tinyusb_config_cdcacm_t config = {
      .cdc_port = TINYUSB_CDC_ACM_0,
      .callback_line_state_changed = line_state_changed,
  };
  esp_err_t result = tinyusb_cdcacm_init(&config);
  if (result != ESP_OK) return result;
  result = tinyusb_console_init(TINYUSB_CDC_ACM_0);
  if (result != ESP_OK) return result;
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  if (xTaskCreatePinnedToCore(receive_task, "usb_rescue", 4096, NULL, 4,
                              &s_rescue_task, 0) != pdPASS)
    return ESP_ERR_NO_MEM;
  s_started = true;
  // 覆盖主机在任务创建前已经打开串口的情况。
  xTaskNotifyGive(s_rescue_task);
  return ESP_OK;
}
