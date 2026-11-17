#include <stdlib.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "heater_board.h"
#include "heater_control.h"
#include "heater_protocol.h"
#include "host_link.h"

static const char *TAG = "heater_and_measure";

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
    QueueHandle_t outbound = xQueueCreate(8, sizeof(heater_message_t));
    if (outbound == NULL) {
        heater_board_requests_off();
        ESP_LOGE(TAG, "Failed to create host output queue");
        abort();
    }
    require_success(heater_control_init(outbound), "Board/ADC initialization");
    require_success(heater_control_start(), "Control task start");
    require_success(host_link_start(outbound), "USB host link start");
    // The IDF scheduler is already running. All business loops block on events;
    // app_main returns while the control and USB tasks continue.
}
