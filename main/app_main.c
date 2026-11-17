#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "device_web.h"
#include "heater_board.h"
#include "heater_control.h"
#include "heater_protocol.h"
#include "host_link.h"
#include "network_manager.h"
#include "telemetry_store.h"

static const char *TAG = "heater_and_measure";
static struct {
    QueueHandle_t samples;
    SemaphoreHandle_t done;
    esp_err_t result;
} s_bootstrap;

static void control_bootstrap(void *arg)
{
    (void)arg;
    // Allocate GPIO/GPTimer interrupts on Core 1, beside the control task.
    s_bootstrap.result = heater_control_init(s_bootstrap.samples);
    if (s_bootstrap.result == ESP_OK) s_bootstrap.result = heater_control_start();
    xSemaphoreGive(s_bootstrap.done);
    vTaskDelete(NULL);
}

static void require_success(esp_err_t result, const char *operation)
{
    if (result != ESP_OK) {
        heater_board_requests_off();
        ESP_LOGE(TAG, "%s failed: %s; outputs disabled", operation, esp_err_to_name(result));
        abort();
    }
}

void app_main(void)
{
    s_bootstrap.samples = xQueueCreate(32, sizeof(heater_message_t));
    s_bootstrap.done = xSemaphoreCreateBinary();
    if (!s_bootstrap.samples || !s_bootstrap.done) {
        heater_board_requests_off();
        ESP_LOGE(TAG, "Failed to create host output queue");
        abort();
    }
    require_success(telemetry_store_init(), "Sample storage initialization");
    // Credential/NVS and initial RF calibration happen before sampling starts.
    require_success(network_manager_init(), "Network initialization");
    require_success(network_manager_start(), "Wi-Fi start");
    require_success(telemetry_store_start(s_bootstrap.samples), "Sample storage task start");
    if (xTaskCreatePinnedToCore(control_bootstrap, "control_init", 6144, NULL, 20, NULL, 1) != pdPASS)
        require_success(ESP_ERR_NO_MEM, "Control initialization task creation");
    if (xSemaphoreTake(s_bootstrap.done, pdMS_TO_TICKS(10000)) != pdTRUE)
        require_success(ESP_ERR_TIMEOUT, "Control initialization timeout");
    if (s_bootstrap.result != ESP_OK) {
        require_success(heater_control_pause(pdMS_TO_TICKS(2000)), "Recovery output shutdown");
        ESP_LOGE(TAG, "Control initialization: %s; outputs off, web/OTA recovery enabled",
                 esp_err_to_name(s_bootstrap.result));
    }
    vSemaphoreDelete(s_bootstrap.done);
    require_success(host_link_start(), "USB rescue start");
    require_success(device_web_start(s_bootstrap.result), "Web service start");
    // The IDF scheduler is already running. All business loops block on events;
    // app_main returns while the control and USB tasks continue.
}
