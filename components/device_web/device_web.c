#include "device_web.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heater_control.h"
#include "heater_protocol.h"
#include "network_manager.h"
#include "ota_update.h"
#include "sdkconfig.h"
#include "telemetry_json.h"
#include "telemetry_store.h"

#define WS_CLIENTS 2
#define WS_BATCH 100
#define HTTP_BATCH 250
#define RECORD_JSON_BYTES 320

static const char *TAG = "device_web";
static httpd_handle_t s_server;
static SemaphoreHandle_t s_operation;
static char s_boot_id[17];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_push_pending;
static bool s_verified;
static esp_err_t s_control_init_result;
static struct { bool active; size_t total, received; esp_err_t error; } s_ota;

extern const char page_start[] asm("_binary_index_html_start");
extern const char page_end[] asm("_binary_index_html_end");

static bool verified(void)
{
    portENTER_CRITICAL(&s_lock);
    const bool result = s_verified;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

static esp_err_t respond(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, body);
}

static bool authorized(httpd_req_t *req)
{
    char header[48];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK &&
        !strncmp(header, "Bearer ", 7) && network_manager_authorized(header + 7)) return true;
    respond(req, "401 Unauthorized", "{\"error\":\"AUTH_REQUIRED\"}");
    return false;
}

static bool query(httpd_req_t *req, char *output, size_t size)
{
    if (httpd_req_get_url_query_len(req) == 0) { output[0] = '\0'; return true; }
    return httpd_req_get_url_query_str(req, output, size) == ESP_OK;
}

static bool cursor_from_query(const char *parameters, uint64_t *from)
{
    char text[32];
    *from = 0;
    if (!strstr(parameters, "from=")) return true;
    return telemetry_form_value(parameters, "from", text, sizeof(text)) && telemetry_parse_cursor(text, from);
}

static char *batch_json(uint64_t from, size_t limit, size_t *length, uint64_t *batch_next)
{
    telemetry_record_t *records = malloc(limit * sizeof(*records));
    const size_t capacity = 512 + limit * RECORD_JSON_BYTES;
    char *output = heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!output) output = malloc(capacity);
    if (!records || !output) { free(records); free(output); return NULL; }
    telemetry_stats_t stats;
    uint64_t next, lost;
    const size_t count = telemetry_store_read(from, records, limit, &next, &lost, &stats);
    if (batch_next) *batch_next = next;
    int used = snprintf(output, capacity,
        "{\"schema\":1,\"boot_id\":\"%s\",\"oldest\":%" PRIu64 ",\"next\":%" PRIu64
        ",\"lost\":%" PRIu64 ",\"more\":%s,\"frames\":[",
        s_boot_id, stats.oldest, next, lost, next < stats.next ? "true" : "false");
    for (size_t i = 0; i < count; ++i) {
        if (i) output[used++] = ',';
        const int size = telemetry_record_json(&records[i], output + used, capacity - used);
        if (size < 0) { free(records); free(output); return NULL; }
        used += size;
    }
    output[used++] = ']'; output[used++] = '}'; output[used] = '\0';
    *length = (size_t)used;
    free(records);
    return output;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    network_status_t network;
    network_manager_status(&network);
    const heater_timing_stats_t timing = heater_control_timing_stats();
    telemetry_stats_t stats = telemetry_store_stats();
    telemetry_record_t record;
    uint64_t next, lost;
    char latest[RECORD_JSON_BYTES] = "null";
    if (stats.next > 1 && telemetry_store_read(stats.next - 1, &record, 1, &next, &lost, &stats))
        telemetry_record_json(&record, latest, sizeof(latest));
    portENTER_CRITICAL(&s_lock);
    const bool active = s_ota.active, verified = s_verified;
    const size_t total = s_ota.total, received = s_ota.received;
    const esp_err_t error = s_ota.error;
    portEXIT_CRITICAL(&s_lock);
    char output[1600];
    snprintf(output, sizeof(output),
        "{\"boot_id\":\"%s\",\"version\":\"%s\",\"verified\":%s,\"lease_ms\":%d,"
        "\"control_ready\":%s,\"control_init_error\":%d,\"maintenance\":%s,\"network\":{\"connected\":%s,\"ap\":%s,\"ip\":\"%s\",\"rssi\":%d,\"ap_ssid\":\"%s\"},"
        "\"store\":{\"psram\":%s,\"capacity\":%u,\"oldest\":%" PRIu64 ",\"next\":%" PRIu64 ",\"overwritten\":%" PRIu64 "},"
        "\"ota\":{\"active\":%s,\"total\":%u,\"received\":%u,\"error\":%d},"
        "\"free_internal_bytes\":%u,\"min_internal_bytes\":%u,\"http_stack_free_bytes\":%u,"
        "\"timing\":{\"adc_start_max_offset_us\":%u,\"adc_start_misses\":%u,\"adc_read_misses\":%u},\"latest\":%s}",
        s_boot_id, esp_app_get_description()->version, verified ? "true" : "false", CONFIG_HEATER_CONTROL_LEASE_MS,
        s_control_init_result == ESP_OK ? "true" : "false", s_control_init_result,
        heater_control_is_maintenance() ? "true" : "false", network.connected ? "true" : "false",
        network.ap_active ? "true" : "false", network.address, network.rssi, network.ap_ssid,
        telemetry_store_uses_psram() ? "true" : "false", (unsigned)stats.capacity, stats.oldest, stats.next, stats.overwritten,
        active ? "true" : "false", (unsigned)total, (unsigned)received, error,
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)uxTaskGetStackHighWaterMark(NULL), (unsigned)timing.adc_start_max_offset_us,
        (unsigned)timing.adc_start_misses, (unsigned)timing.adc_read_misses, latest);
    return respond(req, "200 OK", output);
}

static esp_err_t data_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    char parameters[160], text[32], boot[32];
    uint64_t from, limit = 50;
    if (!query(req, parameters, sizeof(parameters)) || !cursor_from_query(parameters, &from))
        return respond(req, "400 Bad Request", "{\"error\":\"INVALID_CURSOR\"}");
    if (strstr(parameters, "limit=") &&
        (!telemetry_form_value(parameters, "limit", text, sizeof(text)) || !telemetry_parse_cursor(text, &limit)))
        return respond(req, "400 Bad Request", "{\"error\":\"INVALID_LIMIT\"}");
    if (!limit || limit > HTTP_BATCH) return respond(req, "400 Bad Request", "{\"error\":\"LIMIT_1_TO_250\"}");
    if (strstr(parameters, "boot=") &&
        (!telemetry_form_value(parameters, "boot", boot, sizeof(boot)) || strcmp(boot, s_boot_id)))
        return respond(req, "409 Conflict", "{\"error\":\"BOOT_CHANGED\"}");
    size_t length;
    char *payload = batch_json(from, (size_t)limit, &length, NULL);
    if (!payload) return respond(req, "503 Service Unavailable", "{\"error\":\"NO_MEMORY\"}");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t result = httpd_resp_send(req, payload, length);
    free(payload);
    return result;
}

static esp_err_t receive_exact(httpd_req_t *req, void *buffer, size_t size)
{
    size_t received = 0;
    while (received < size) {
        const int count = httpd_req_recv(req, (char *)buffer + received, size - received);
        // One socket timeout aborts the transfer; an idle uploader cannot hold maintenance forever.
        if (count <= 0) return ESP_ERR_TIMEOUT;
        received += (size_t)count;
    }
    return ESP_OK;
}

static esp_err_t command_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    if (!req->content_len || req->content_len > HEATER_COMMAND_MAX_LENGTH)
        return respond(req, "400 Bad Request", "{\"error\":\"INVALID_COMMAND\"}");
    char body[HEATER_COMMAND_MAX_LENGTH];
    if (receive_exact(req, body, req->content_len) != ESP_OK) return ESP_FAIL;
    heater_command_t command;
    if (!heater_command_parse(body, req->content_len, &command))
        return respond(req, "400 Bad Request", "{\"error\":\"INVALID_COMMAND\"}");
    if (!verified() && command.kind == HEATER_COMMAND_SET)
        return respond(req, "409 Conflict", "{\"error\":\"SELF_TEST_PENDING\"}");
    if (heater_control_submit(&command) != ESP_OK)
        return respond(req, "409 Conflict", "{\"error\":\"MAINTENANCE\"}");
    return respond(req, "200 OK", command.kind == HEATER_COMMAND_SET ? "{\"ack\":\"SET\"}" :
        command.kind == HEATER_COMMAND_STOP ? "{\"ack\":\"STOP\"}" : "{\"ack\":\"PING\"}");
}

static esp_err_t resume_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    if (!verified() || xSemaphoreTake(s_operation, 0) != pdTRUE)
        return respond(req, "409 Conflict", "{\"error\":\"BUSY\"}");
    const esp_err_t result = heater_control_resume();
    xSemaphoreGive(s_operation);
    return respond(req, result == ESP_OK ? "200 OK" : "409 Conflict",
                   result == ESP_OK ? "{\"ack\":\"RESUMED_ZERO_DUTY\"}" : "{\"error\":\"RESUME_FAILED\"}");
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    if (xSemaphoreTake(s_operation, 0) != pdTRUE)
        return respond(req, "409 Conflict", "{\"error\":\"BUSY\"}");
    const esp_err_t result = heater_control_pause(pdMS_TO_TICKS(2000));
    if (result != ESP_OK) {
        xSemaphoreGive(s_operation);
        return respond(req, "409 Conflict", "{\"error\":\"PAUSE_FAILED\"}");
    }
    respond(req, "200 OK", "{\"ack\":\"REBOOT\"}");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

typedef struct { uint64_t next; } ws_session_t;

static esp_err_t ws_auth(httpd_req_t *req)
{
    char parameters[160], token[33];
    if (!query(req, parameters, sizeof(parameters)) ||
        !telemetry_form_value(parameters, "token", token, sizeof(token)) || !network_manager_authorized(token)) {
        respond(req, "401 Unauthorized", "{\"error\":\"AUTH_REQUIRED\"}");
        return ESP_FAIL;
    }
    size_t count = 8;
    int clients[8];
    unsigned websocket_count = 0;
    if (httpd_get_client_list(s_server, &count, clients) != ESP_OK) return ESP_FAIL;
    for (size_t i = 0; i < count; ++i)
        websocket_count += httpd_ws_get_fd_info(s_server, clients[i]) == HTTPD_WS_CLIENT_WEBSOCKET;
    if (websocket_count >= WS_CLIENTS) {
        respond(req, "503 Service Unavailable", "{\"error\":\"TWO_VIEWERS_MAX\"}");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ws_connected(httpd_req_t *req)
{
    // IDF 6.1 does not call the URI handler for an upgrade handshake.
    // Initialize the per-client cursor in its explicit completion callback.
    ws_session_t *session = calloc(1, sizeof(*session));
    if (!session) return ESP_ERR_NO_MEM;
    char parameters[160], boot[32];
    if (!query(req, parameters, sizeof(parameters)) || !cursor_from_query(parameters, &session->next)) {
        free(session); return ESP_FAIL;
    }
    if (telemetry_form_value(parameters, "boot", boot, sizeof(boot)) && strcmp(boot, s_boot_id)) session->next = 0;
    req->sess_ctx = session;
    req->free_ctx = free;
    // Bound the time a stalled viewer can delay other HTTP requests.
    const struct timeval timeout = {.tv_usec = 200000};
    setsockopt(httpd_req_to_sockfd(req), SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    return ESP_OK;
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    httpd_ws_frame_t frame = {0};
    if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) return ESP_FAIL;
    // Commands use the authenticated HTTP API, keeping control separate from data streaming.
    if (frame.len > 64) return ESP_FAIL;
    uint8_t input[65];
    frame.payload = input;
    return httpd_ws_recv_frame(req, &frame, frame.len);
}

static void push_work(void *arg)
{
    (void)arg;
    size_t count = 8;
    int clients[8];
    if (httpd_get_client_list(s_server, &count, clients) == ESP_OK) {
        for (size_t i = 0; i < count; ++i) {
            if (httpd_ws_get_fd_info(s_server, clients[i]) != HTTPD_WS_CLIENT_WEBSOCKET) continue;
            ws_session_t *session = httpd_sess_get_ctx(s_server, clients[i]);
            if (!session) continue;
            size_t length;
            uint64_t batch_next;
            char *payload = batch_json(session->next, WS_BATCH, &length, &batch_next);
            if (!payload) continue;
            httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)payload, .len = length};
            const esp_err_t sent = httpd_ws_send_frame_async(s_server, clients[i], &frame);
            free(payload);
            if (sent != ESP_OK) httpd_sess_trigger_close(s_server, clients[i]);
            // The client's durable cursor is supplied explicitly after reconnect.
            // Do not infer that delivery to a socket means persistence on the host.
            else session->next = batch_next;
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_push_pending = false;
    portEXIT_CRITICAL(&s_lock);
}

static void push_task(void *arg)
{
    (void)arg;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));
        portENTER_CRITICAL(&s_lock);
        const bool queue = !s_push_pending;
        s_push_pending = true;
        portEXIT_CRITICAL(&s_lock);
        if (queue && httpd_queue_work(s_server, push_work, NULL) != ESP_OK) {
            portENTER_CRITICAL(&s_lock); s_push_pending = false; portEXIT_CRITICAL(&s_lock);
        }
    }
}

static int ota_read(void *context, void *buffer, size_t size)
{
    return httpd_req_recv(context, buffer, size);
}

static void ota_progress(void *context, size_t received)
{
    (void)context;
    portENTER_CRITICAL(&s_lock);
    s_ota.received = received;
    portEXIT_CRITICAL(&s_lock);
}

static void ota_worker(void *arg)
{
    httpd_req_t *req = arg;
    const esp_err_t result = ota_update_receive(req->content_len, ota_read, ota_progress, req,
                                               s_control_init_result == ESP_OK);
    portENTER_CRITICAL(&s_lock);
    s_ota.error = result; s_ota.active = false;
    portEXIT_CRITICAL(&s_lock);
    if (result == ESP_OK) respond(req, "200 OK", "{\"ack\":\"OTA\",\"reboot\":true}");
    else respond(req, "400 Bad Request", "{\"error\":\"OTA_FAILED\",\"resume_required\":true}");
    httpd_req_async_handler_complete(req);
    ESP_LOGI(TAG, "OTA result: %s; worker stack free: %u bytes", esp_err_to_name(result),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    if (result == ESP_OK) { vTaskDelay(pdMS_TO_TICKS(500)); esp_restart(); }
    xSemaphoreGive(s_operation);
    vTaskDelete(NULL);
}

static esp_err_t ota_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (!partition || req->content_len < 512 || req->content_len > partition->size)
        return respond(req, "400 Bad Request", "{\"error\":\"INVALID_IMAGE_SIZE\"}");
    if (!verified() || xSemaphoreTake(s_operation, 0) != pdTRUE)
        return respond(req, "409 Conflict", "{\"error\":\"BUSY_OR_SELF_TEST_PENDING\"}");
    httpd_req_t *copy;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        xSemaphoreGive(s_operation); return ESP_FAIL;
    }
    portENTER_CRITICAL(&s_lock);
    s_ota.active = true; s_ota.total = req->content_len; s_ota.received = 0; s_ota.error = ESP_OK;
    portEXIT_CRITICAL(&s_lock);
    if (xTaskCreatePinnedToCore(ota_worker, "ota_writer", 6144, copy, 5, NULL, 0) != pdPASS) {
        respond(copy, "503 Service Unavailable", "{\"error\":\"NO_MEMORY\"}");
        httpd_req_async_handler_complete(copy);
        portENTER_CRITICAL(&s_lock); s_ota.active = false; portEXIT_CRITICAL(&s_lock);
        xSemaphoreGive(s_operation);
    }
    return ESP_OK;
}

static esp_err_t wifi_handler(httpd_req_t *req)
{
    if (!authorized(req)) return ESP_OK;
    if (!verified() || xSemaphoreTake(s_operation, 0) != pdTRUE)
        return respond(req, "409 Conflict", "{\"error\":\"BUSY\"}");
    char body[400], ssid[33], password[65];
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (req->content_len && req->content_len < sizeof(body) &&
        receive_exact(req, body, req->content_len) == ESP_OK) {
        body[req->content_len] = '\0';
        if (telemetry_form_value(body, "ssid", ssid, sizeof(ssid)) &&
            telemetry_form_value(body, "password", password, sizeof(password))) {
            result = heater_control_pause(pdMS_TO_TICKS(2000));
            if (result == ESP_OK) result = network_manager_save(ssid, password);
        }
    }
    if (result == ESP_OK) {
        respond(req, "200 OK", "{\"ack\":\"WIFI_SAVED\",\"reboot\":true}");
        vTaskDelay(pdMS_TO_TICKS(500)); esp_restart();
    }
    xSemaphoreGive(s_operation);
    return respond(req, "400 Bad Request", "{\"error\":\"WIFI_SAVE_FAILED\"}");
}

static void verify_task(void *arg)
{
    (void)arg;
    unsigned good = 0;
    uint32_t sequence = 0;
    for (unsigned i = 0; s_control_init_result == ESP_OK && i < 100 && good < 10; ++i) {
        heater_telemetry_t latest;
        if (telemetry_store_latest(&latest) && latest.sequence != sequence) {
            sequence = latest.sequence;
            good = latest.valid_mask == 15 && !(latest.faults & (HEATER_FAULT_TIMING | HEATER_FAULT_SPI | HEATER_FAULT_PWM)) ? good + 1 : 0;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    // Router availability is independent of firmware health. Driver/server init
    // already succeeded. Use local ADC/control health to validate a new image.
    if (xSemaphoreTake(s_operation, pdMS_TO_TICKS(2000)) != pdTRUE) abort();
    const esp_err_t result = ota_update_confirm(good >= 10, s_control_init_result == ESP_OK);
    portENTER_CRITICAL(&s_lock); s_verified = result == ESP_OK; portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Startup confirmation: %s; ADC %s; stack free %u bytes",
             esp_err_to_name(result), good >= 10 ? "healthy" : "unhealthy",
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(s_operation);
    vTaskDelete(NULL);
}

esp_err_t device_web_start(esp_err_t control_init_result)
{
    s_control_init_result = control_init_result;
    snprintf(s_boot_id, sizeof(s_boot_id), "%08" PRIx32 "%08" PRIx32, esp_random(), esp_random());
    // The HTTP handler acquires this gate and hands the operation to the OTA
    // task. A mutex cannot be released by a different task; use an unowned
    // binary semaphore so failure cleanup cannot assert and reboot the device.
    s_operation = xSemaphoreCreateBinary();
    if (!s_operation) return ESP_ERR_NO_MEM;
    xSemaphoreGive(s_operation);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = 0;
    config.task_priority = 4;
    config.stack_size = 10240;
    config.max_uri_handlers = 9;
    config.max_open_sockets = 6;
    config.recv_wait_timeout = 2;
    config.send_wait_timeout = 1;
    config.lru_purge_enable = true;
    esp_err_t result = httpd_start(&s_server, &config);
    if (result != ESP_OK) return result;
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = index_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/data", .method = HTTP_GET, .handler = data_handler},
        {.uri = "/api/control", .method = HTTP_POST, .handler = command_handler},
        {.uri = "/api/resume", .method = HTTP_POST, .handler = resume_handler},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = wifi_handler},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_handler},
        {.uri = "/ws", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true,
         .ws_pre_handshake_cb = ws_auth, .ws_post_handshake_cb = ws_connected},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        result = httpd_register_uri_handler(s_server, &routes[i]);
        if (result != ESP_OK) return result;
    }
    if (xTaskCreatePinnedToCore(push_task, "web_push", 4096, NULL, 3, NULL, 0) != pdPASS ||
        xTaskCreatePinnedToCore(verify_task, "boot_verify", 4096, NULL, 5, NULL, 0) != pdPASS) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "HTTP dashboard ready on port 80; 1 Hz batches, up to 2 viewers");
    return ESP_OK;
}
