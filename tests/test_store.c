// 独立链接生产记录器，验证停止代次和历史元数据的完整保留。
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>

#include "telemetry_store.h"

static void (*s_entry)(void*);
static void* s_arg;
static jmp_buf s_done;
static heater_message_t s_message;
static bool s_available;

bool esp_psram_is_initialized(void) { return false; }
void* heap_caps_malloc(size_t size, unsigned caps) {
  (void)caps;
  return malloc(size);
}
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void*)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t timeout) {
  (void)mutex;
  (void)timeout;
  return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex) {
  (void)mutex;
  return pdTRUE;
}
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void*), const char* name,
                                   uint32_t stack, void* arg,
                                   UBaseType_t priority, TaskHandle_t* task,
                                   BaseType_t core) {
  (void)name;
  (void)task;
  assert(core == 0 && stack == 4096 && priority == 6);
  s_entry = entry;
  s_arg = arg;
  return pdPASS;
}
BaseType_t xQueueReceive(QueueHandle_t queue, void* output,
                         TickType_t timeout) {
  (void)queue;
  (void)timeout;
  if (!s_available) longjmp(s_done, 1);
  *(heater_message_t*)output = s_message;
  s_available = false;
  return pdTRUE;
}
static void consume(uint32_t config_id, uint32_t generation) {
  s_message =
      (heater_message_t){.run_id = 1,
                         .generation = generation,
                         .data = {.experiment = {.config_id = config_id}}};
  s_available = true;
  if (!setjmp(s_done)) s_entry(s_arg);
}
int main(void) {
  assert(telemetry_store_init() == ESP_OK);
  assert(!telemetry_store_uses_psram());
  assert(telemetry_store_start((void*)2) == ESP_OK);
  telemetry_store_set_run(true, 1, 1);
  for (uint32_t id = 1; id <= HEATER_CONFIG_CAPACITY; ++id) {
    const heater_config_record_t record = {.config_id = id};
    assert(telemetry_store_prepare_config(&record) == ESP_OK);
    consume(id, 1);
  }
  heater_config_record_t configurations[HEATER_CONFIG_CAPACITY];
  assert(telemetry_store_configurations(
             configurations, HEATER_CONFIG_CAPACITY) == HEATER_CONFIG_CAPACITY);
  const heater_config_record_t next = {.config_id = HEATER_CONFIG_CAPACITY + 1};
  assert(telemetry_store_prepare_config(&next) == ESP_ERR_NO_MEM);
  const uint64_t before = telemetry_store_stats().next;
  telemetry_store_set_run(false, 1, 2);
  consume(1, 1);
  telemetry_store_sync_stop();
  assert(telemetry_store_stats().next == before);
  telemetry_store_set_run(true, 1, 3);
  consume(1, 1);
  assert(telemetry_store_stats().next == before);
  for (size_t i = 0; i < telemetry_store_stats().capacity; ++i) consume(64, 3);
  assert(telemetry_store_prepare_config(&next) == ESP_OK);
  puts("记录器停止代次、配置容量拒绝和历史覆盖后的元数据回收通过");
  return 0;
}
