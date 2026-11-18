// 采样自检及版本确认属于启动流程，网页仅查询结果。
#include <stdlib.h>

#include "device_maintenance.h"
#include "esp_log.h"
#include "freertos/task.h"
#include "heater_runtime.h"
#include "ota_update.h"
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_verified;
static esp_err_t s_control_init_result;
static const char* TAG = "boot_verify";
static void verify_task(void* arg) {
  (void)arg;
  unsigned good = 0;
  uint32_t sequence = 0;
  for (unsigned i = 0; s_control_init_result == ESP_OK && i < 100 && good < 10;
       ++i) {
    heater_telemetry_t latest;
    if (heater_runtime_latest(&latest) && latest.sequence != sequence) {
      sequence = latest.sequence;
      good = latest.valid_mask == 15 &&
                     !(latest.faults & (HEATER_FAULT_TIMING | HEATER_FAULT_SPI |
                                        HEATER_FAULT_PWM))
                 ? good + 1
                 : 0;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // 网络驱动及服务初始化已成功，路由器在线与否不作为固件健康判据。
  // 使用本地 ADC/控制采样快照完成新版本确认。
  if (!device_operation_take(pdMS_TO_TICKS(2000))) abort();
  const esp_err_t result =
      ota_update_confirm(good >= 10, s_control_init_result == ESP_OK);
  portENTER_CRITICAL(&s_lock);
  s_verified = result == ESP_OK;
  portEXIT_CRITICAL(&s_lock);
  ESP_LOGI(TAG, "Startup confirmation: %s; ADC %s; stack free %u bytes",
           esp_err_to_name(result), good >= 10 ? "healthy" : "unhealthy",
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  device_operation_give();
  vTaskDelete(NULL);
}

bool device_maintenance_verified(void) {
  portENTER_CRITICAL(&s_lock);
  const bool verified = s_verified;
  portEXIT_CRITICAL(&s_lock);
  return verified;
}
esp_err_t device_maintenance_verify_start(esp_err_t control_init_result) {
  s_control_init_result = control_init_result;
  if (xTaskCreatePinnedToCore(verify_task, "boot_verify", 4096, NULL, 5, NULL,
                              0) != pdPASS)
    return ESP_ERR_NO_MEM;
  return ESP_OK;
}
