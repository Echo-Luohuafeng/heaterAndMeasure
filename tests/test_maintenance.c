// 独立链接维护服务，验证整个写入期间的操作门闩和任务所有权。
#include <assert.h>
#include <stdio.h>

#include "device_credentials.h"
#include "device_maintenance.h"

static bool s_operation_free, s_flash_free, s_paused, s_pause_fails;
static TaskHandle_t s_task = (void*)3;
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void*)1; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
  s_flash_free = true;
  return (void*)2;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) {
  (void)timeout;
  bool* free = mutex == (void*)1 ? &s_operation_free : &s_flash_free;
  if (!*free) return pdFALSE;
  *free = false;
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
  bool* free = mutex == (void*)1 ? &s_operation_free : &s_flash_free;
  assert(!*free);
  *free = true;
  return pdTRUE;
}
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return s_task; }
esp_err_t device_credentials_init(void) { return ESP_OK; }
esp_err_t heater_runtime_pause(TickType_t timeout) {
  assert(timeout);
  s_paused = true;
  return s_pause_fails ? ESP_ERR_TIMEOUT : ESP_OK;
}
esp_err_t heater_runtime_resume(void) {
  assert(s_paused);
  s_paused = false;
  return ESP_OK;
}
int main(void) {
  assert(device_maintenance_init() == ESP_OK);
  assert(device_flash_begin(10) == ESP_ERR_INVALID_STATE);
  assert(device_operation_take(0));
  assert(device_flash_begin(10) == ESP_OK && s_paused);
  assert(!device_operation_take(0));
  assert(device_flash_begin(10) == ESP_ERR_TIMEOUT);
  s_task = (void*)4;
  assert(device_flash_end(true) == ESP_ERR_INVALID_STATE);
  s_task = (void*)3;
  assert(device_flash_end(false) == ESP_OK && s_paused);
  device_operation_give();
  assert(device_operation_take(0));
  s_pause_fails = true;
  assert(device_flash_begin(10) == ESP_ERR_TIMEOUT && s_flash_free && s_paused);
  s_pause_fails = false;
  assert(device_flash_begin(10) == ESP_OK);
  assert(device_flash_end(true) == ESP_OK && !s_paused);
  device_operation_give();
  puts("维护事务互斥、暂停失败及任务所有权测试通过");
  return 0;
}
