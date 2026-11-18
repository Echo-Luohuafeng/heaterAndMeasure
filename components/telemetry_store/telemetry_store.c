#include "telemetry_store.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define STORE_BYTES (4U * 1024U * 1024U)
#define FALLBACK_BYTES (32U * 1024U)

static telemetry_ring_t s_ring;
static SemaphoreHandle_t s_lock;
static bool s_psram;
static bool s_started;
// 接收门只位于内部 RAM；锁顺序为 ring mutex 后 gate lock，不回调实时内核。
static portMUX_TYPE s_gate_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
  bool running;
  uint32_t run_id, generation;
} s_gate;
// 配置容量固定；仍有历史引用的配置不回收，容量不足明确拒绝新配置。
typedef struct {
  heater_config_record_t record;
  uint32_t references;
  uint64_t created;
  bool pinned;
} configuration_slot_t;
static configuration_slot_t* s_configs;

// 创建缓存互斥锁并分配历史内存，将实际可用字节数换算为完整记录容量。
// 优先 PSRAM，失败时使用内部 RAM 降级缓存，记录启动容量诊断。
esp_err_t telemetry_store_init(void) {
  if (s_lock != NULL) return ESP_ERR_INVALID_STATE;
  const size_t metadata_bytes = HEATER_CONFIG_CAPACITY * sizeof(*s_configs);
  if (esp_psram_is_initialized())
    s_configs =
        heap_caps_malloc(metadata_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!s_configs)
    s_configs =
        heap_caps_malloc(metadata_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!s_configs) return ESP_ERR_NO_MEM;
  memset(s_configs, 0, metadata_bytes);
  s_lock = xSemaphoreCreateMutex();
  if (!s_lock) return ESP_ERR_NO_MEM;
  telemetry_record_t* memory = NULL;
  size_t bytes = STORE_BYTES;
  if (esp_psram_is_initialized()) {
    memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_psram = memory != NULL;
  }
  if (!memory) {
    bytes = FALLBACK_BYTES;
    memory = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  }
  if (!telemetry_ring_init(&s_ring, memory, bytes / sizeof(*memory)))
    return ESP_ERR_NO_MEM;
  ESP_LOGI("telemetry_store", "%s: %u records (%u bytes), volatile history",
           s_psram ? "PSRAM" : "internal RAM fallback",
           (unsigned)s_ring.capacity, (unsigned)bytes);
  return ESP_OK;
}

// 阻塞接收 `arg` 指定的非空采样队列，在缓存锁内核验当前运行 ID 后写入历史。
// sample_store 位于 Core 0，优先级 6，栈为 4096 字节，不阻塞高优先级采样任务。
static void store_task(void* arg) {
  const QueueHandle_t samples = arg;
  heater_message_t message;
  for (;;) {
    if (xQueueReceive(samples, &message, portMAX_DELAY) != pdPASS) continue;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    portENTER_CRITICAL(&s_gate_lock);
    const bool accepted = s_gate.running && s_gate.run_id == message.run_id &&
                          s_gate.generation == message.generation;
    portEXIT_CRITICAL(&s_gate_lock);
    unsigned config_slot = HEATER_CONFIG_CAPACITY;
    for (unsigned i = 0; i < HEATER_CONFIG_CAPACITY; ++i)
      if (s_configs[i].record.config_id == message.data.experiment.config_id) {
        config_slot = i;
        break;
      }
    if (accepted && config_slot < HEATER_CONFIG_CAPACITY) {
      // 环形槽覆盖时解除旧配置引用，然后增加新配置引用。
      if (s_ring.next > s_ring.capacity) {
        const uint32_t old_id =
            s_ring.records[(s_ring.next - 1) % s_ring.capacity]
                .frame.experiment.config_id;
        for (unsigned i = 0; i < HEATER_CONFIG_CAPACITY; ++i)
          if (s_configs[i].record.config_id == old_id &&
              s_configs[i].references)
            --s_configs[i].references;
      }
      telemetry_ring_push(&s_ring, &message.data);
      ++s_configs[config_slot].references;
    }
    xSemaphoreGive(s_lock);
  }
}

// 获取并立即释放缓存锁，形成 STOP 确认前的写入完成屏障。
void telemetry_store_sync_stop(void) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  xSemaphoreGive(s_lock);
}

// 检查初始化及启动状态后创建缓存任务，向任务传递长期有效的队列句柄。
esp_err_t telemetry_store_start(QueueHandle_t samples) {
  if (!samples || !s_lock || s_started) return ESP_ERR_INVALID_STATE;
  if (xTaskCreatePinnedToCore(store_task, "sample_store", 4096, samples, 6,
                              NULL, 0) != pdPASS)
    return ESP_ERR_NO_MEM;
  s_started = true;
  return ESP_OK;
}

// 读取初始化时固定的 PSRAM 分配结果，无需访问环形缓存或加锁。
bool telemetry_store_uses_psram(void) { return s_psram; }

// 在缓存互斥锁内计算统计信息，防止并发写入产生不一致游标。
telemetry_stats_t telemetry_store_stats(void) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const telemetry_stats_t result = telemetry_ring_stats(&s_ring);
  xSemaphoreGive(s_lock);
  return result;
}

// 在同一次缓存锁保护下获取统计和批次，保证 lost/next 与复制记录一致。
size_t telemetry_store_read(uint64_t from, telemetry_record_t* output,
                            size_t limit, uint64_t* next, uint64_t* lost,
                            telemetry_stats_t* stats) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  *stats = telemetry_ring_stats(&s_ring);
  const size_t count =
      telemetry_ring_read(&s_ring, from, output, limit, next, lost);
  xSemaphoreGive(s_lock);
  return count;
}

// 在缓存锁内按最后写入槽复制历史帧，空缓存时不触碰输出。
bool telemetry_store_latest(heater_telemetry_t* frame) {
  xSemaphoreTake(s_lock, portMAX_DELAY);
  const bool present = s_ring.next > 1;
  if (present)
    *frame = s_ring.records[(s_ring.next - 2) % s_ring.capacity].frame;
  xSemaphoreGive(s_lock);
  return present;
}

// 可从任务或 ISR 调用；不接触 PSRAM、不阻塞、不分配。
void IRAM_ATTR telemetry_store_set_run(bool running, uint32_t run_id,
                                       uint32_t generation) {
  // 同一回调由任务和 ISR 调用，SAFE 宏按当前上下文选择正确临界区。
  portENTER_CRITICAL_SAFE(&s_gate_lock);
  s_gate.running = running;
  s_gate.run_id = run_id;
  s_gate.generation = generation;
  portEXIT_CRITICAL_SAFE(&s_gate_lock);
}
esp_err_t telemetry_store_prepare_config(const heater_config_record_t* record) {
  if (!record || !record->config_id || !s_lock) return ESP_ERR_INVALID_ARG;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  unsigned slot = HEATER_CONFIG_CAPACITY;
  for (unsigned i = 0; i < HEATER_CONFIG_CAPACITY; ++i)
    if (s_configs[i].record.config_id == record->config_id) {
      slot = i;
      break;
    }
  if (slot == HEATER_CONFIG_CAPACITY) {
    for (unsigned i = 0; i < HEATER_CONFIG_CAPACITY; ++i) {
      // 多保留 64 帧，覆盖长度 32 的待消费队列与周期发布中的帧。
      if (!s_configs[i].record.config_id ||
          (!s_configs[i].pinned && !s_configs[i].references &&
           s_ring.next >= s_configs[i].created + 64)) {
        slot = i;
        break;
      }
    }
    if (slot == HEATER_CONFIG_CAPACITY) {
      xSemaphoreGive(s_lock);
      return ESP_ERR_NO_MEM;
    }
    s_configs[slot].record = *record;
    s_configs[slot].references = 0;
    s_configs[slot].created = s_ring.next;
  }
  for (unsigned i = 0; i < HEATER_CONFIG_CAPACITY; ++i)
    s_configs[i].pinned = i == slot;
  xSemaphoreGive(s_lock);
  return ESP_OK;
}
unsigned telemetry_store_configurations(heater_config_record_t* records,
                                        unsigned capacity) {
  if (!records || !s_lock) return 0;
  unsigned count = 0;
  xSemaphoreTake(s_lock, portMAX_DELAY);
  for (unsigned i = 0; i < HEATER_CONFIG_CAPACITY && count < capacity; ++i)
    if (s_configs[i].record.config_id) records[count++] = s_configs[i].record;
  xSemaphoreGive(s_lock);
  return count;
}
