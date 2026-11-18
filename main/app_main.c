// ESP32-S3 四路 PT100 加热与四线电阻测量固件的启动入口。
// 每 20 ms 先执行 15 ms 加热，再在剩余窗口切换并采样；USB 网络提供控制、历史和
// OTA。 默认空闲，显式 START 后加热及记录；通道许可、租约与 USB STOP
// 提供停热保护。

#include <stdlib.h>

#include "device_maintenance.h"
#include "device_usb.h"
#include "device_web.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heater_board.h"
#include "heater_protocol.h"
#include "heater_runtime.h"
#include "telemetry_store.h"

static const char* TAG = "heater_and_measure";
// 保存跨核控制初始化的队列、完成信号量与结果；静态生命周期覆盖初始化任务。
static struct {
  QueueHandle_t samples;
  SemaphoreHandle_t done;
  esp_err_t result;
} s_bootstrap;

// 在 Core 1 初始化并启动控制任务，使 GPIO/GPTimer 中断与控制任务同核。
// `arg` 未使用；结果经静态启动上下文和信号量返回，完成后删除自身。
static void control_bootstrap(void* arg) {
  (void)arg;
  // 在 Core 1 分配 GPIO/GPTimer 中断，与控制任务同核。
  s_bootstrap.result = heater_runtime_init(s_bootstrap.samples);
  if (s_bootstrap.result == ESP_OK) s_bootstrap.result = heater_runtime_start();
  xSemaphoreGive(s_bootstrap.done);
  vTaskDelete(NULL);
}

// 检查启动步骤的 `result`，失败时关闭请求、记录 `operation` 并终止启动。
// `operation` 须为非空字符串，仅用于诊断。
static void require_success(esp_err_t result, const char* operation) {
  if (result != ESP_OK) {
    heater_board_requests_off();
    ESP_LOGE(TAG, "%s failed: %s; outputs disabled", operation,
             esp_err_to_name(result));
    abort();
  }
}

// 创建通信对象，依次启动历史缓存、网络、控制与救援/网页服务。
// 控制初始化失败时先确认输出关断，再开放网络恢复；其余关键失败终止启动。
// 完成后返回，由 ESP-IDF 已启动的 FreeRTOS 调度各业务任务。
void app_main(void) {
  s_bootstrap.samples = xQueueCreate(32, sizeof(heater_message_t));
  s_bootstrap.done = xSemaphoreCreateBinary();
  if (!s_bootstrap.samples || !s_bootstrap.done) {
    heater_board_requests_off();
    ESP_LOGE(TAG, "Failed to create host output queue");
    abort();
  }
  require_success(device_maintenance_init(), "Maintenance initialization");
  heater_runtime_set_run_callback(telemetry_store_set_run);
  require_success(telemetry_store_init(), "Sample storage initialization");
  // 凭据/NVS 写入在采样启动前完成。
  require_success(device_usb_init(), "Network initialization");
  require_success(device_usb_start(), "USB network start");
  require_success(device_usb_console_start(), "USB rescue start");
  require_success(telemetry_store_start(s_bootstrap.samples),
                  "Sample storage task start");
  if (xTaskCreatePinnedToCore(control_bootstrap, "control_init", 6144, NULL, 20,
                              NULL, 1) != pdPASS)
    require_success(ESP_ERR_NO_MEM, "Control initialization task creation");
  if (xSemaphoreTake(s_bootstrap.done, pdMS_TO_TICKS(10000)) != pdTRUE)
    require_success(ESP_ERR_TIMEOUT, "Control initialization timeout");
  if (s_bootstrap.result != ESP_OK) {
    require_success(heater_runtime_pause(pdMS_TO_TICKS(2000)),
                    "Recovery output shutdown");
    ESP_LOGE(
        TAG,
        "Control initialization: %s; outputs off, web/OTA recovery enabled",
        esp_err_to_name(s_bootstrap.result));
  }
  vSemaphoreDelete(s_bootstrap.done);
  require_success(device_maintenance_verify_start(s_bootstrap.result),
                  "Boot verification task start");
  require_success(device_web_start(s_bootstrap.result), "Web service start");
  // IDF 调度器已经运行，业务循环通过阻塞等待事件让出 CPU。
  // app_main 返回后，控制、网络与 USB 等任务继续执行。
}
