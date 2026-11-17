#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ota_update.h"
#include "esp_app_format.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "nvs.h"

static uint8_t image[9000], written[9000];
static size_t read_offset, written_size, interrupt_at, progress_size;
static bool paused, pause_fails, selected, no_memory, end_fails;
static unsigned writes, fail_write, aborts, begins;
static const esp_partition_t partition = {.size = 4 * 1024 * 1024};
static const esp_partition_t current_partition = {.size = 4 * 1024 * 1024};
static esp_ota_img_states_t boot_state;
static esp_err_t boot_state_result;
static bool baseline, marked_valid, rollback;
static uint8_t required_sampling;
static const esp_app_desc_t running = {.magic_word = ESP_APP_DESC_MAGIC_WORD, .project_name = "heater_and_measure"};

const esp_partition_t *esp_ota_get_next_update_partition(const esp_partition_t *current)
{ (void)current; return &partition; }
const esp_app_desc_t *esp_app_get_description(void) { return &running; }
const esp_partition_t *esp_ota_get_running_partition(void) { return &current_partition; }
esp_err_t esp_ota_get_state_partition(const esp_partition_t *target, esp_ota_img_states_t *state)
{ assert(target == &current_partition); *state = boot_state; return boot_state_result; }
esp_err_t esp_ota_mark_app_valid_cancel_rollback(void)
{ assert(paused && !rollback); marked_valid = true; return ESP_OK; }
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot(void)
{ assert(paused); rollback = true; return ESP_FAIL; }
esp_err_t heater_control_resume(void) { assert(paused); paused = false; return ESP_OK; }
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle)
{ assert(paused && !strcmp(name, "heater_boot")); (void)mode; *handle = 1; return ESP_OK; }
esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{ assert(paused && handle == 1 && !strcmp(key, "require_adc")); required_sampling = value; return ESP_OK; }
esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{ assert(paused && handle == 1 && !strcmp(key, "require_adc")); *value = required_sampling; return ESP_OK; }
esp_err_t nvs_commit(nvs_handle_t handle) { assert(paused && handle == 1); return ESP_OK; }
void nvs_close(nvs_handle_t handle) { assert(handle == 1); }
esp_err_t heater_control_pause(TickType_t timeout)
{ assert(timeout > 0); paused = true; return pause_fails ? ESP_ERR_TIMEOUT : ESP_OK; }
void *heap_caps_malloc(size_t size, unsigned caps)
{ assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)); return no_memory ? NULL : malloc(size); }
void vTaskDelay(TickType_t ticks) { assert(ticks > 0); }
esp_err_t esp_ota_begin(const esp_partition_t *target, size_t size, esp_ota_handle_t *handle)
{ assert(paused && !pause_fails && target == &partition && size == OTA_WITH_SEQUENTIAL_WRITES); ++begins; *handle = 1; return ESP_OK; }
esp_err_t esp_ota_write(esp_ota_handle_t handle, const void *data, size_t size)
{
    assert(paused && handle == 1);
    if (++writes == fail_write) return ESP_FAIL;
    assert(written_size + size <= sizeof(written));
    memcpy(written + written_size, data, size); written_size += size; return ESP_OK;
}
esp_err_t esp_ota_end(esp_ota_handle_t handle)
{ assert(paused && handle == 1 && written_size == sizeof(image)); return end_fails ? ESP_FAIL : ESP_OK; }
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *target)
{
    assert(paused && !end_fails);
    if (target == &current_partition) baseline = true;
    else { assert(target == &partition && written_size == sizeof(image)); selected = true; }
    return ESP_OK;
}
esp_err_t esp_ota_abort(esp_ota_handle_t handle) { assert(handle == 1); ++aborts; return ESP_OK; }
static int read_data(void *ctx, void *output, size_t size)
{
    (void)ctx;
    if (read_offset >= interrupt_at) return 0;
    if (size > 13) size = 13; // exercise fragmented HTTP/TCP reads
    if (size > interrupt_at - read_offset) size = interrupt_at - read_offset;
    assert(read_offset + size <= sizeof(image));
    memcpy(output, image + read_offset, size); read_offset += size; return (int)size;
}
static void progress(void *ctx, size_t size) { (void)ctx; assert(size >= progress_size); progress_size = size; }
static void reset(void)
{
    memset(image, 0x35, sizeof(image));
    const esp_image_header_t header = {.magic = ESP_IMAGE_HEADER_MAGIC, .chip_id = ESP_CHIP_ID_ESP32S3};
    memcpy(image, &header, sizeof(header));
    memcpy(image + sizeof(header) + sizeof(esp_image_segment_header_t), &running, sizeof(running));
    read_offset = written_size = progress_size = 0; interrupt_at = sizeof(image);
    paused = pause_fails = selected = no_memory = end_fails = false;
    writes = fail_write = aborts = begins = 0;
    boot_state = ESP_OTA_IMG_VALID; boot_state_result = ESP_OK;
    baseline = marked_valid = rollback = false;
    required_sampling = 1;
}
int main(void)
{
    reset();
    assert(ota_update_receive(sizeof(image), read_data, progress, NULL, true) == ESP_OK);
    assert(paused && selected && !aborts && progress_size == sizeof(image));
    assert(!memcmp(image, written, sizeof(image)));
    reset(); pause_fails = true;
    assert(ota_update_receive(sizeof(image), read_data, progress, NULL, true) == ESP_ERR_TIMEOUT);
    assert(!begins && !selected);
    reset(); interrupt_at = 1000;
    assert(ota_update_receive(sizeof(image), read_data, progress, NULL, true) == ESP_ERR_TIMEOUT);
    assert(paused && begins == 1 && aborts == 1 && !selected);
    reset(); image[0] = 0;
    assert(ota_update_receive(sizeof(image), read_data, progress, NULL, true) == ESP_ERR_INVALID_ARG && !begins);
    reset(); image[12] = 1;
    assert(ota_update_receive(sizeof(image), read_data, progress, NULL, true) == ESP_ERR_INVALID_ARG && !begins);
    reset(); image[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + 48] = 'X';
    assert(ota_update_receive(sizeof(image), read_data, progress, NULL, true) == ESP_ERR_INVALID_ARG && !begins);
    reset(); fail_write = 2;
    assert(ota_update_receive(sizeof(image), read_data, progress, NULL, true) == ESP_FAIL && aborts == 1 && !selected);
    reset(); end_fails = true;
    assert(ota_update_receive(sizeof(image), read_data, progress, NULL, true) == ESP_FAIL && !selected && !aborts);
    reset(); no_memory = true;
    assert(ota_update_receive(sizeof(image), read_data, progress, NULL, true) == ESP_ERR_NO_MEM && paused && !begins);
    reset();
    assert(ota_update_receive(partition.size + 1, read_data, progress, NULL, true) == ESP_ERR_INVALID_ARG && !paused);
    reset(); boot_state_result = ESP_ERR_NOT_FOUND;
    assert(ota_update_confirm(false, true) == ESP_OK && baseline && marked_valid && !paused);
    reset(); boot_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(ota_update_confirm(true, true) == ESP_OK && marked_valid && !paused && !rollback);
    reset(); boot_state = ESP_OTA_IMG_PENDING_VERIFY;
    assert(ota_update_confirm(false, true) == ESP_FAIL && rollback && paused && !marked_valid);
    reset(); boot_state = ESP_OTA_IMG_PENDING_VERIFY; pause_fails = true;
    assert(ota_update_confirm(true, true) == ESP_ERR_TIMEOUT && !marked_valid && !baseline);
    reset();
    assert(ota_update_confirm(false, true) == ESP_OK && !paused && !marked_valid);
    reset(); boot_state = ESP_OTA_IMG_PENDING_VERIFY; required_sampling = 0;
    assert(ota_update_confirm(false, false) == ESP_OK && marked_valid && paused && !rollback);
    reset();
    assert(ota_update_receive(sizeof(image), read_data, progress, NULL, false) == ESP_OK);
    assert(required_sampling == 0 && selected);
    puts("OTA fragmentation, flash gating, disconnect, image validation and failure recovery tests passed");
    return 0;
}
