#pragma once
#include "fake_idf.h"
typedef struct { size_t size; } esp_partition_t;
typedef unsigned esp_ota_handle_t;
#define OTA_WITH_SEQUENTIAL_WRITES 0xfffffffe
const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *);
esp_err_t esp_ota_begin(const esp_partition_t *, size_t, esp_ota_handle_t *);
esp_err_t esp_ota_write(esp_ota_handle_t, const void *, size_t);
esp_err_t esp_ota_end(esp_ota_handle_t);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *);
esp_err_t esp_ota_abort(esp_ota_handle_t);
typedef enum { ESP_OTA_IMG_UNDEFINED, ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID } esp_ota_img_states_t;
const esp_partition_t *esp_ota_get_running_partition(void);
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *);
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void);
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void);
