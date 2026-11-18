// 维护服务持有完整 Flash 事务；不能只查询 paused 标志后直接写入。
#include "device_maintenance.h"

#include "device_credentials.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heater_runtime.h"

static SemaphoreHandle_t s_operation;
static SemaphoreHandle_t s_flash;
static TaskHandle_t s_flash_owner;
static portMUX_TYPE s_operation_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_operation_taken;

esp_err_t device_maintenance_init(void) {
  if (s_operation) return ESP_ERR_INVALID_STATE;
  s_operation = xSemaphoreCreateBinary();
  s_flash = xSemaphoreCreateMutex();
  if (!s_operation || !s_flash) return ESP_ERR_NO_MEM;
  xSemaphoreGive(s_operation);
  // 唯一不需要运行时暂停的写入路径：控制和采样尚未启动。
  return device_credentials_init();
}
bool device_operation_take(TickType_t timeout) {
  if (!s_operation || xSemaphoreTake(s_operation, timeout) != pdTRUE)
    return false;
  portENTER_CRITICAL(&s_operation_lock);
  s_operation_taken = true;
  portEXIT_CRITICAL(&s_operation_lock);
  return true;
}
void device_operation_give(void) {
  portENTER_CRITICAL(&s_operation_lock);
  s_operation_taken = false;
  portEXIT_CRITICAL(&s_operation_lock);
  xSemaphoreGive(s_operation);
}
esp_err_t device_flash_begin(TickType_t timeout) {
  portENTER_CRITICAL(&s_operation_lock);
  const bool operation = s_operation_taken;
  portEXIT_CRITICAL(&s_operation_lock);
  if (!operation) return ESP_ERR_INVALID_STATE;
  if (!s_flash || xSemaphoreTake(s_flash, timeout) != pdTRUE)
    return ESP_ERR_TIMEOUT;
  const esp_err_t result = heater_runtime_pause(timeout);
  if (result != ESP_OK) {
    xSemaphoreGive(s_flash);
    return result;
  }
  s_flash_owner = xTaskGetCurrentTaskHandle();
  return ESP_OK;
}
esp_err_t device_flash_end(bool resume_sampling) {
  if (!s_flash_owner || s_flash_owner != xTaskGetCurrentTaskHandle())
    return ESP_ERR_INVALID_STATE;
  const esp_err_t result = resume_sampling ? heater_runtime_resume() : ESP_OK;
  s_flash_owner = NULL;
  xSemaphoreGive(s_flash);
  return result;
}
