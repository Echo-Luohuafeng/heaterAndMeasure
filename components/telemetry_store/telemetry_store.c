#include "telemetry_store.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "heater_control.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define STORE_BYTES (4U * 1024U * 1024U)
#define FALLBACK_BYTES (32U * 1024U)

static telemetry_ring_t s_ring;
static SemaphoreHandle_t s_lock;
static bool s_psram;
static bool s_started;

esp_err_t telemetry_store_init(void)
{
    if (s_lock != NULL) return ESP_ERR_INVALID_STATE;
    s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return ESP_ERR_NO_MEM;
    telemetry_record_t *memory = NULL;
    size_t bytes = STORE_BYTES;
    if (esp_psram_is_initialized()) {
        memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        s_psram = memory != NULL;
    }
    if (!memory) {
        bytes = FALLBACK_BYTES;
        memory = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!telemetry_ring_init(&s_ring, memory, bytes / sizeof(*memory))) return ESP_ERR_NO_MEM;
    ESP_LOGI("telemetry_store", "%s: %u records (%u bytes), volatile history",
             s_psram ? "PSRAM" : "internal RAM fallback", (unsigned)s_ring.capacity, (unsigned)bytes);
    return ESP_OK;
}

static void store_task(void *arg)
{
    const QueueHandle_t samples = arg;
    heater_message_t message;
    for (;;) {
        if (xQueueReceive(samples, &message, portMAX_DELAY) != pdPASS) continue;
        if (message.kind != HEATER_MESSAGE_DATA) continue;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const heater_run_state_t run = heater_control_run_state();
        if (telemetry_message_recordable(&message, run.running, run.id))
            telemetry_ring_push(&s_ring, &message.data);
        xSemaphoreGive(s_lock);
    }
}

void telemetry_store_sync_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    xSemaphoreGive(s_lock);
}

esp_err_t telemetry_store_start(QueueHandle_t samples)
{
    if (!samples || !s_lock || s_started) return ESP_ERR_INVALID_STATE;
    if (xTaskCreatePinnedToCore(store_task, "sample_store", 4096, samples, 6, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    s_started = true;
    return ESP_OK;
}

bool telemetry_store_uses_psram(void) { return s_psram; }

telemetry_stats_t telemetry_store_stats(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const telemetry_stats_t result = telemetry_ring_stats(&s_ring);
    xSemaphoreGive(s_lock);
    return result;
}

size_t telemetry_store_read(uint64_t from, telemetry_record_t *output, size_t limit,
                            uint64_t *next, uint64_t *lost, telemetry_stats_t *stats)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    *stats = telemetry_ring_stats(&s_ring);
    const size_t count = telemetry_ring_read(&s_ring, from, output, limit, next, lost);
    xSemaphoreGive(s_lock);
    return count;
}

bool telemetry_store_latest(heater_telemetry_t *frame)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool present = s_ring.next > 1;
    if (present) *frame = s_ring.records[(s_ring.next - 2) % s_ring.capacity].frame;
    xSemaphoreGive(s_lock);
    return present;
}
