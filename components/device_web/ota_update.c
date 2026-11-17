#include "ota_update.h"

#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heater_control.h"
#include "nvs.h"

#define OTA_BUFFER_BYTES 4096

static esp_err_t read_exact(ota_read_fn read, void *context, void *buffer, size_t size)
{
    size_t done = 0;
    while (done < size) {
        const int count = read(context, (char *)buffer + done, size - done);
        if (count <= 0 || (size_t)count > size - done) return ESP_ERR_TIMEOUT;
        done += (size_t)count;
    }
    return ESP_OK;
}

esp_err_t ota_update_receive(size_t total, ota_read_fn read, ota_progress_fn progress,
                             void *context, bool require_sampling)
{
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || !read || total < 512 || total > partition->size) return ESP_ERR_INVALID_ARG;
    esp_err_t result = heater_control_pause(pdMS_TO_TICKS(2000));
    if (result != ESP_OK) return result;
    uint8_t *buffer = heap_caps_malloc(OTA_BUFFER_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buffer) return ESP_ERR_NO_MEM;
    const size_t prefix_size = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    result = read_exact(read, context, buffer, prefix_size);
    if (result == ESP_OK) {
        esp_image_header_t header;
        esp_app_desc_t description;
        memcpy(&header, buffer, sizeof(header));
        memcpy(&description, buffer + sizeof(header) + sizeof(esp_image_segment_header_t), sizeof(description));
        if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32S3 ||
            description.magic_word != ESP_APP_DESC_MAGIC_WORD ||
            memcmp(description.project_name, esp_app_get_description()->project_name, sizeof(description.project_name)))
            result = ESP_ERR_INVALID_ARG;
    }
    esp_ota_handle_t handle = 0;
    if (result == ESP_OK) {
        // Updates from a healthy sampler must retain sampling health. A device
        // already in hardware recovery may update its network/recovery firmware
        // before ADC power/wiring is restored. NVS makes this survive power loss.
        nvs_handle_t settings;
        result = nvs_open("heater_boot", NVS_READWRITE, &settings);
        if (result == ESP_OK) {
            result = nvs_set_u8(settings, "require_adc", require_sampling ? 1 : 0);
            if (result == ESP_OK) result = nvs_commit(settings);
            nvs_close(settings);
        }
    }
    if (result == ESP_OK) result = esp_ota_begin(partition, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (result == ESP_OK) result = esp_ota_write(handle, buffer, prefix_size);
    size_t received = prefix_size;
    if (result == ESP_OK && progress) progress(context, received);
    while (result == ESP_OK && received < total) {
        const size_t size = total - received < OTA_BUFFER_BYTES ? total - received : OTA_BUFFER_BYTES;
        result = read_exact(read, context, buffer, size);
        if (result == ESP_OK) result = esp_ota_write(handle, buffer, size);
        if (result == ESP_OK) {
            received += size;
            if (progress) progress(context, received);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (result == ESP_OK) {
        result = esp_ota_end(handle);
        handle = 0; // end releases the handle even when verification fails
    }
    if (result == ESP_OK) result = esp_ota_set_boot_partition(partition);
    if (handle) esp_ota_abort(handle);
    free(buffer);
    return result;
}

esp_err_t ota_update_confirm(bool healthy, bool controller_ready)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    const esp_err_t state_result = esp_ota_get_state_partition(running, &state);
    const bool pending = state_result == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY;
    const bool first_boot = state_result == ESP_ERR_NOT_FOUND ||
                            (state_result == ESP_OK && state == ESP_OTA_IMG_UNDEFINED);
    if (state_result != ESP_OK && state_result != ESP_ERR_NOT_FOUND) return state_result;
    if (!pending && !first_boot) return state == ESP_OTA_IMG_VALID ? ESP_OK : ESP_ERR_INVALID_STATE;
    esp_err_t result = heater_control_pause(pdMS_TO_TICKS(2000));
    if (result != ESP_OK) return result;
    uint8_t require_sampling = 1;
    if (pending) {
        nvs_handle_t settings;
        result = nvs_open("heater_boot", NVS_READONLY, &settings);
        if (result == ESP_OK) {
            result = nvs_get_u8(settings, "require_adc", &require_sampling);
            nvs_close(settings);
        }
        if (result == ESP_ERR_NVS_NOT_FOUND) result = ESP_OK;
        if (result != ESP_OK || require_sampling > 1) return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }
    if (pending && !healthy && require_sampling) {
        result = esp_ota_mark_app_invalid_rollback_and_reboot();
        // Successful rollback reboots. A returned call never justifies marking
        // the failing application valid, even if a mock/port returns ESP_OK.
        return result == ESP_OK ? ESP_FAIL : result;
    }
    if (first_boot) result = esp_ota_set_boot_partition(running);
    if (result == ESP_OK) result = esp_ota_mark_app_valid_cancel_rollback();
    if (result == ESP_OK && controller_ready) result = heater_control_resume();
    return result;
}
