#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define APP_TASK_STACK_SIZE_BYTES 4096
#define APP_TASK_PRIORITY (tskIDLE_PRIORITY + 1)
#define APP_TASK_IDLE_DELAY_MS 1000

static const char *TAG = "heater_and_measure";

static void app_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "Application task started");

    for (;;) {
        // Placeholder for application coordination; business modules belong in components/.
        vTaskDelay(pdMS_TO_TICKS(APP_TASK_IDLE_DELAY_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Initializing FreeRTOS application");

    // ESP-IDF task stack sizes are specified in bytes. Leave core affinity to the scheduler.
    const BaseType_t result = xTaskCreate(app_task, "app_task", APP_TASK_STACK_SIZE_BYTES,
                                         NULL, APP_TASK_PRIORITY, NULL);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create application task");
        abort();
    }

    // ESP-IDF deletes its main task after app_main returns; app_task continues running.
}
