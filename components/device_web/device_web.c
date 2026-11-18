#include "device_web.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "cJSON.h"
#include "device_credentials.h"
#include "device_maintenance.h"
#include "device_usb.h"
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
#include "heater_controller.h"
#include "heater_protocol.h"
#include "heater_service.h"
#include "ota_update.h"
#include "sdkconfig.h"
#include "telemetry_json.h"
#include "telemetry_store.h"

#define WS_CLIENTS 2
#define WS_BATCH 100
#define HTTP_BATCH 250
#define RECORD_JSON_BYTES 768

static const char* TAG = "device_web";
static httpd_handle_t s_server;
static char s_boot_id[17];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_push_pending;
static esp_err_t s_control_init_result;
// 保存网页状态接口可读的 OTA 进度与错误，所有运行期访问由 s_lock 保护。
static struct {
  bool active;
  size_t total, received;
  esp_err_t error;
} s_ota;

extern const char page_start[] asm("_binary_index_html_start");
extern const char page_end[] asm("_binary_index_html_end");
extern const char dashboard_start[] asm("_binary_dashboard_js_start");
extern const char dashboard_end[] asm("_binary_dashboard_js_end");
extern const char api_start[] asm("_binary_api_js_start");
extern const char api_end[] asm("_binary_api_js_end");
extern const char plot_start[] asm("_binary_plot_js_start");
extern const char plot_end[] asm("_binary_plot_js_end");
extern const char stream_start[] asm("_binary_stream_js_start");
extern const char stream_end[] asm("_binary_stream_js_end");
extern const char control_start[] asm("_binary_control_js_start");
extern const char control_end[] asm("_binary_control_js_end");
extern const char export_start[] asm("_binary_export_js_start");
extern const char export_end[] asm("_binary_export_js_end");
extern const char style_start[] asm("_binary_dashboard_css_start");
extern const char style_end[] asm("_binary_dashboard_css_end");

// 向非空的 `req` 发送禁用缓存的 JSON 响应，status/body 均须为非空字符串。
// 返回 HTTP 服务的发送结果，响应内容由调用方提供。
static esp_err_t respond(httpd_req_t* req, const char* status,
                         const char* body) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_sendstr(req, body);
}

// 校验请求的 Bearer 口令，成功返回 true，否则先回复 401 再返回 false。
// `req` 须为有效 HTTP 请求，失败后处理器无需再次发送响应。
static bool authorized(httpd_req_t* req) {
  char header[48];
  if (httpd_req_get_hdr_value_str(req, "Authorization", header,
                                  sizeof(header)) == ESP_OK &&
      !strncmp(header, "Bearer ", 7) &&
      device_credentials_authorized(header + 7))
    return true;
  respond(req, "401 Unauthorized", "{\"error\":\"AUTH_REQUIRED\"}");
  return false;
}

// 将 URL 查询串写入非空的 `output`，没有查询串时写入空字符串。
// `req` 须有效且 `size` 非零；查询读取失败或缓冲不足时返回 false。
static bool query(httpd_req_t* req, char* output, size_t size) {
  if (httpd_req_get_url_query_len(req) == 0) {
    output[0] = '\0';
    return true;
  }
  return httpd_req_get_url_query_str(req, output, size) == ESP_OK;
}

// 从非空的查询字符串 `parameters` 解析 from，缺省时将非空的输出 `from` 设为零。
// 字段非法时返回 false，调用方应拒绝该请求。
static bool cursor_from_query(const char* parameters, uint64_t* from) {
  char text[32];
  *from = 0;
  if (!strstr(parameters, "from=")) return true;
  return telemetry_form_value(parameters, "from", text, sizeof(text)) &&
         telemetry_parse_cursor(text, from);
}

// 从 `from` 读取至多 `limit` 条历史并构造含启动 ID、丢失数及续传游标的 JSON。
// `length` 须非空，`batch_next` 可为 NULL；结果优先使用 PSRAM，由调用方
// free()。 分配或编码失败返回
// NULL；缓存读取只在复制期间持锁，序列化在锁外完成。
static char* batch_json(uint64_t from, size_t limit, size_t* length,
                        uint64_t* batch_next) {
  telemetry_record_t* records = malloc(limit * sizeof(*records));
  const size_t capacity = 512 + limit * RECORD_JSON_BYTES;
  char* output =
      heap_caps_malloc(capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!output) output = malloc(capacity);
  if (!records || !output) {
    free(records);
    free(output);
    return NULL;
  }
  telemetry_stats_t stats;
  uint64_t next, lost;
  const size_t count =
      telemetry_store_read(from, records, limit, &next, &lost, &stats);
  if (batch_next) *batch_next = next;
  int used = snprintf(output, capacity,
                      "{\"schema\":2,\"boot_id\":\"%s\",\"oldest\":%" PRIu64
                      ",\"next\":%" PRIu64 ",\"lost\":%" PRIu64
                      ",\"more\":%s,\"frames\":[",
                      s_boot_id, stats.oldest, next, lost,
                      next < stats.next ? "true" : "false");
  for (size_t i = 0; i < count; ++i) {
    if (i) output[used++] = ',';
    const int size =
        telemetry_record_json(&records[i], output + used, capacity - used);
    if (size < 0) {
      free(records);
      free(output);
      return NULL;
    }
    used += size;
  }
  output[used++] = ']';
  output[used++] = '}';
  output[used] = '\0';
  *length = (size_t)used;
  free(records);
  return output;
}

// 向有效的 `req` 返回随固件嵌入的 UTF-8 网页，不要求 API 口令。
// 长度排除嵌入资源的结尾 NUL，返回发送结果。
static esp_err_t index_handler(httpd_req_t* req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  return httpd_resp_send(req, page_start, page_end - page_start - 1);
}

// 嵌入资源由 IDF 生成；应用只返回内容与 MIME 类型。
static esp_err_t asset_handler(httpd_req_t* req) {
  const struct {
    const char *uri, *start, *end, *type;
  } assets[] = {
      {"/dashboard.js", dashboard_start, dashboard_end,
       "text/javascript; charset=utf-8"},
      {"/api.js", api_start, api_end, "text/javascript; charset=utf-8"},
      {"/plot.js", plot_start, plot_end, "text/javascript; charset=utf-8"},
      {"/stream.js", stream_start, stream_end,
       "text/javascript; charset=utf-8"},
      {"/control.js", control_start, control_end,
       "text/javascript; charset=utf-8"},
      {"/export.js", export_start, export_end,
       "text/javascript; charset=utf-8"},
      {"/dashboard.css", style_start, style_end, "text/css; charset=utf-8"},
  };
  for (unsigned i = 0; i < sizeof(assets) / sizeof(assets[0]); ++i) {
    if (!strcmp(req->uri, assets[i].uri)) {
      httpd_resp_set_type(req, assets[i].type);
      httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
      return httpd_resp_send(req, assets[i].start,
                             assets[i].end - assets[i].start - 1);
    }
  }
  return ESP_ERR_NOT_FOUND;
}

// 鉴权后汇总运行、网络、历史、OTA、内存/栈与时序状态，返回 JSON。
// `req` 须有效；latest 取历史记录，启动自检使用独立的控制安全快照。
static esp_err_t status_handler(httpd_req_t* req) {
  if (!authorized(req)) return ESP_OK;
  network_status_t network;
  device_usb_status(&network);
  const heater_timing_stats_t timing = heater_service_timing_stats();
  const heater_run_state_t run = heater_service_run_state();
  telemetry_stats_t stats = telemetry_store_stats();
  telemetry_record_t record;
  uint64_t next, lost;
  char latest[RECORD_JSON_BYTES] = "null";
  if (stats.next > 1 &&
      telemetry_store_read(stats.next - 1, &record, 1, &next, &lost, &stats))
    telemetry_record_json(&record, latest, sizeof(latest));
  portENTER_CRITICAL(&s_lock);
  const bool active = s_ota.active, verified = device_maintenance_verified();
  const size_t total = s_ota.total, received = s_ota.received;
  const esp_err_t error = s_ota.error;
  portEXIT_CRITICAL(&s_lock);
  const heater_controller_status_t controller =
      heater_service_controller_status();
  char controller_json[1500];
  const controller_config_t* c = &controller.config;
  char params[900] = "{}";
  unsigned count;
  const controller_descriptor_t* descriptors =
      heater_controller_registry(&count);
  for (unsigned a = 0; a < count; ++a)
    if (descriptors[a].id == c->algorithm) {
      cJSON* object = cJSON_CreateObject();
      if (!object) return ESP_ERR_NO_MEM;
      for (unsigned n = 0; n < descriptors[a].parameter_count; ++n) {
        const controller_parameter_t* p = &descriptors[a].parameters[n];
        if (p->count == 1)
          cJSON_AddNumberToObject(object, p->name, c->parameters[p->offset]);
        else
          cJSON_AddItemToObject(
              object, p->name,
              cJSON_CreateFloatArray(c->parameters + p->offset, p->count));
      }
      const bool printed =
          cJSON_PrintPreallocated(object, params, sizeof(params), false);
      cJSON_Delete(object);
      if (!printed) return ESP_ERR_NO_MEM;
    }
  snprintf(controller_json, sizeof(controller_json),
           "{\"mode\":\"%s\",\"config_id\":%u,\"algorithm\":%u,\"parameters\":%"
           "s,\"ready\":%s,\"state\":%u,"
           "\"target\":[%.6f,%.6f,%.6f,%.6f],\"tolerance_ohm\":%.6f,\"max_"
           "duty\":%.3f,"
           "\"delay_cycles\":2,\"worker_stack_free_bytes\":%u,\"faults\":%u,"
           "\"lease_refreshes\":%u,\"lease_remaining_ms\":%u}",
           controller.resistance_mode ? "resistance" : "manual",
           (unsigned)controller.config_id, (unsigned)c->algorithm, params,
           controller.ready ? "true" : "false", (unsigned)controller.state,
           (double)c->target[0], (double)c->target[1], (double)c->target[2],
           (double)c->target[3], (double)c->tolerance_ohm, (double)c->max_duty,
           (unsigned)controller.worker_stack_free_bytes,
           (unsigned)controller.controller_faults,
           (unsigned)controller.lease_refreshes,
           (unsigned)controller.lease_remaining_ms);
  char measurement[RECORD_JSON_BYTES] = "null";
  heater_telemetry_t current;
  if (heater_service_measurement(&current)) {
    const telemetry_record_t sample = {.cursor = 0, .frame = current};
    telemetry_record_json(&sample, measurement, sizeof(measurement));
  }
  char output[4000];
  snprintf(
      output, sizeof(output),
      "{\"boot_id\":\"%s\",\"version\":\"%s\",\"verified\":%s,\"lease_ms\":%d,"
      "\"max_duty\":%d,"
      "\"running\":%s,\"recording\":%s,\"run_id\":%u,"
      "\"control_ready\":%s,\"control_init_error\":%d,\"maintenance\":%s,"
      "\"network\":{\"connected\":%s,\"transport\":\"usb_ncm\",\"ip\":\"%s\","
      "\"rx_drops\":%u,\"tx_drops\":%u,\"usb_stack_free_bytes\":%u,\"network_"
      "stack_free_bytes\":%u},"
      "\"store\":{\"psram\":%s,\"capacity\":%u,\"oldest\":%" PRIu64
      ",\"next\":%" PRIu64 ",\"overwritten\":%" PRIu64
      "},"
      "\"ota\":{\"active\":%s,\"total\":%u,\"received\":%u,\"error\":%d},"
      "\"free_internal_bytes\":%u,\"min_internal_bytes\":%u,\"http_stack_free_"
      "bytes\":%u,"
      "\"timing\":{\"adc_start_max_offset_us\":%u,\"adc_start_misses\":%u,"
      "\"adc_read_misses\":%u},\"latest\":%s,\"measurement\":%s,\"measurement_"
      "age_us\":%llu,\"controller\":%s}",
      s_boot_id, esp_app_get_description()->version,
      verified ? "true" : "false", CONFIG_HEATER_CONTROL_LEASE_MS,
      HEATER_MAX_DUTY, run.running ? "true" : "false",
      run.running ? "true" : "false", (unsigned)run.id,
      s_control_init_result == ESP_OK ? "true" : "false", s_control_init_result,
      heater_service_is_maintenance() ? "true" : "false",
      network.connected ? "true" : "false", network.address,
      (unsigned)network.rx_drops, (unsigned)network.tx_drops,
      (unsigned)network.usb_stack_free_bytes,
      (unsigned)network.network_stack_free_bytes,
      telemetry_store_uses_psram() ? "true" : "false", (unsigned)stats.capacity,
      stats.oldest, stats.next, stats.overwritten, active ? "true" : "false",
      (unsigned)total, (unsigned)received, error,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL |
                                                MALLOC_CAP_8BIT),
      (unsigned)uxTaskGetStackHighWaterMark(NULL),
      (unsigned)timing.adc_start_max_offset_us,
      (unsigned)timing.adc_start_misses, (unsigned)timing.adc_read_misses,
      latest, measurement,
      (unsigned long long)heater_service_measurement_age_us(), controller_json);
  return respond(req, "200 OK", output);
}

// 鉴权并校验 from/limit/boot，返回最多 250 条历史；启动 ID 不符时回复冲突。
// `req` 须有效；批次缓冲在发送后释放，分配失败回复服务不可用。
static esp_err_t data_handler(httpd_req_t* req) {
  if (!authorized(req)) return ESP_OK;
  char parameters[160], text[32], boot[32];
  uint64_t from, limit = 50;
  if (!query(req, parameters, sizeof(parameters)) ||
      !cursor_from_query(parameters, &from))
    return respond(req, "400 Bad Request", "{\"error\":\"INVALID_CURSOR\"}");
  if (strstr(parameters, "limit=") &&
      (!telemetry_form_value(parameters, "limit", text, sizeof(text)) ||
       !telemetry_parse_cursor(text, &limit)))
    return respond(req, "400 Bad Request", "{\"error\":\"INVALID_LIMIT\"}");
  if (!limit || limit > HTTP_BATCH)
    return respond(req, "400 Bad Request", "{\"error\":\"LIMIT_1_TO_250\"}");
  if (strstr(parameters, "boot=") &&
      (!telemetry_form_value(parameters, "boot", boot, sizeof(boot)) ||
       strcmp(boot, s_boot_id)))
    return respond(req, "409 Conflict", "{\"error\":\"BOOT_CHANGED\"}");
  size_t length;
  char* payload = batch_json(from, (size_t)limit, &length, NULL);
  if (!payload)
    return respond(req, "503 Service Unavailable", "{\"error\":\"NO_MEMORY\"}");
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  const esp_err_t result = httpd_resp_send(req, payload, length);
  free(payload);
  return result;
}

// 阻塞读取有效请求 `req` 的恰好 `size` 字节到非空的 `buffer`。
// 任一次接收超时或连接关闭即返回 ESP_ERR_TIMEOUT，成功返回 ESP_OK。
static esp_err_t receive_exact(httpd_req_t* req, void* buffer, size_t size) {
  size_t received = 0;
  while (received < size) {
    const int count =
        httpd_req_recv(req, (char*)buffer + received, size - received);
    // 任一次 socket 超时即终止接收，避免空闲上传者长期占用维护流程。
    if (count <= 0) return ESP_ERR_TIMEOUT;
    received += (size_t)count;
  }
  return ESP_OK;
}

// 鉴权并解析控制命令；STOP 立即执行并等待缓存屏障，其余操作需完成启动自检。
// `req` 须有效；START 可恢复维护后的控制任务，回复新运行 ID 和历史起始游标。
static esp_err_t command_handler(httpd_req_t* req) {
  if (!authorized(req)) return ESP_OK;
  if (!req->content_len || req->content_len > HEATER_COMMAND_MAX_LENGTH)
    return respond(req, "400 Bad Request", "{\"error\":\"INVALID_COMMAND\"}");
  char body[HEATER_COMMAND_MAX_LENGTH];
  if (receive_exact(req, body, req->content_len) != ESP_OK) return ESP_FAIL;
  heater_command_t command;
  if (!heater_command_parse(body, req->content_len, &command))
    return respond(req, "400 Bad Request", "{\"error\":\"INVALID_COMMAND\"}");
  if (command.kind == HEATER_COMMAND_STOP) {
    if (heater_service_submit(&command) != ESP_OK)
      return respond(req, "409 Conflict",
                     "{\"error\":\"CONTROL_UNAVAILABLE\"}");
    return respond(req, "200 OK", "{\"ack\":\"STOP\"}");
  }
  const uint64_t next = telemetry_store_stats().next;
  const esp_err_t result = heater_service_submit(&command);
  const heater_run_state_t run = heater_service_run_state();
  if (result != ESP_OK)
    return respond(req, "409 Conflict", "{\"error\":\"CONTROL_UNAVAILABLE\"}");
  if (command.kind == HEATER_COMMAND_START ||
      command.kind == HEATER_COMMAND_RESISTANCE_START) {
    char reply[128];
    snprintf(reply, sizeof(reply),
             "{\"ack\":\"START\",\"run_id\":%u,\"next\":%" PRIu64 "}",
             (unsigned)run.id, next);
    return respond(req, "200 OK", reply);
  }
  return respond(req, "200 OK",
                 command.kind == HEATER_COMMAND_SET ? "{\"ack\":\"SET\"}"
                                                    : "{\"ack\":\"PING\"}");
}

// 所有新增路由仍使用原 Bearer token 和 OTA 操作门闩。
static esp_err_t controllers_handler(httpd_req_t* req) {
  if (!authorized(req)) return ESP_OK;
  unsigned count;
  const controller_descriptor_t* items = heater_controller_registry(&count);
  cJSON *root = cJSON_CreateObject(), *algorithms = cJSON_CreateArray();
  if (!root || !algorithms) {
    cJSON_Delete(root);
    cJSON_Delete(algorithms);
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddItemToObject(root, "algorithms", algorithms);
  for (unsigned i = 0; i < count; ++i) {
    cJSON* item = cJSON_CreateObject();
    if (!item) {
      cJSON_Delete(root);
      return ESP_ERR_NO_MEM;
    }
    cJSON_AddNumberToObject(item, "id", items[i].id);
    cJSON_AddStringToObject(item, "name", items[i].name);
    cJSON_AddStringToObject(item, "version", items[i].version);
    cJSON* params = cJSON_AddArrayToObject(item, "parameters");
    for (unsigned j = 0; j < items[i].parameter_count; ++j) {
      const controller_parameter_t* p = &items[i].parameters[j];
      cJSON* v = cJSON_CreateObject();
      cJSON_AddStringToObject(v, "name", p->name);
      cJSON_AddNumberToObject(v, "count", p->count);
      cJSON_AddNumberToObject(v, "min", p->minimum);
      cJSON_AddNumberToObject(v, "max", p->maximum);
      cJSON_AddNumberToObject(v, "default", p->default_value);
      cJSON_AddItemToArray(params, v);
    }
    cJSON_AddItemToArray(algorithms, item);
  }
  char* body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!body) return ESP_ERR_NO_MEM;
  const esp_err_t result = respond(req, "200 OK", body);
  free(body);
  return result;
}
static esp_err_t experiments_handler(httpd_req_t* req) {
  if (!authorized(req)) return ESP_OK;
  // 配置快照放在堆上，避免 64 份配置占满 HTTP 任务栈。
  heater_config_record_t* records =
      malloc(HEATER_CONFIG_CAPACITY * sizeof(*records));
  if (!records) return ESP_ERR_NO_MEM;
  const unsigned count =
      telemetry_store_configurations(records, HEATER_CONFIG_CAPACITY);
  unsigned algorithms;
  const controller_descriptor_t* registry =
      heater_controller_registry(&algorithms);
  cJSON* root = cJSON_CreateObject();
  if (!root) {
    free(records);
    return ESP_ERR_NO_MEM;
  }
  cJSON_AddStringToObject(root, "boot_id", s_boot_id);
  cJSON_AddStringToObject(root, "firmware", esp_app_get_description()->version);
  cJSON_AddNumberToObject(root, "period_us", HEATER_PERIOD_US);
  cJSON_AddNumberToObject(root, "delay_cycles", CONTROLLER_DELAY_CYCLES);
  const float references[4] = {
      CONFIG_HEATER_REFERENCE_1_MOHM, CONFIG_HEATER_REFERENCE_2_MOHM,
      CONFIG_HEATER_REFERENCE_3_MOHM, CONFIG_HEATER_REFERENCE_4_MOHM};
  const float offsets[4] = {
      CONFIG_HEATER_OFFSET_1_MOHM, CONFIG_HEATER_OFFSET_2_MOHM,
      CONFIG_HEATER_OFFSET_3_MOHM, CONFIG_HEATER_OFFSET_4_MOHM};
  cJSON_AddItemToObject(root, "reference_mohm",
                        cJSON_CreateFloatArray(references, 4));
  cJSON_AddItemToObject(root, "offset_mohm",
                        cJSON_CreateFloatArray(offsets, 4));
  cJSON* array = cJSON_AddArrayToObject(root, "configurations");
  for (unsigned i = 0; i < count; ++i) {
    const controller_config_t* c = &records[i].config;
    cJSON* record = cJSON_CreateObject();
    cJSON_AddNumberToObject(record, "config_id", records[i].config_id);
    cJSON_AddNumberToObject(record, "mode", records[i].mode);
    cJSON* manual = cJSON_AddArrayToObject(record, "manual_duty");
    for (unsigned channel = 0; channel < 4; ++channel)
      cJSON_AddItemToArray(
          manual, cJSON_CreateNumber(records[i].manual_ticks[channel] * 0.5));
    cJSON_AddNumberToObject(record, "algorithm", c->algorithm);
    cJSON_AddItemToObject(record, "target",
                          cJSON_CreateFloatArray(c->target, 4));
    cJSON_AddNumberToObject(record, "max_duty", c->max_duty);
    cJSON_AddNumberToObject(record, "tolerance_ohm", c->tolerance_ohm);
    cJSON_AddStringToObject(record, "algorithm_version",
                            records[i].algorithm_version);
    float reference[4], offset[4];
    for (unsigned channel = 0; channel < 4; ++channel) {
      reference[channel] = records[i].reference_mohm[channel];
      offset[channel] = records[i].offset_mohm[channel];
    }
    cJSON_AddItemToObject(record, "reference_mohm",
                          cJSON_CreateFloatArray(reference, 4));
    cJSON_AddItemToObject(record, "offset_mohm",
                          cJSON_CreateFloatArray(offset, 4));
    cJSON* params = cJSON_AddObjectToObject(record, "parameters");
    for (unsigned j = 0; j < algorithms; ++j)
      if (registry[j].id == c->algorithm) {
        for (unsigned k = 0; k < registry[j].parameter_count; ++k) {
          const controller_parameter_t* p = &registry[j].parameters[k];
          if (p->count == 1)
            cJSON_AddNumberToObject(params, p->name, c->parameters[p->offset]);
          else
            cJSON_AddItemToObject(
                params, p->name,
                cJSON_CreateFloatArray(c->parameters + p->offset, p->count));
        }
      }
    cJSON_AddItemToArray(array, record);
  }
  char* body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  free(records);
  if (!body) return ESP_ERR_NO_MEM;
  const esp_err_t result = respond(req, "200 OK", body);
  free(body);
  return result;
}
static bool json_four(const cJSON* root, const char* key, float output[4]) {
  const cJSON* array = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) != 4) return false;
  for (unsigned i = 0; i < 4; ++i) {
    const cJSON* n = cJSON_GetArrayItem(array, i);
    if (!cJSON_IsNumber(n) || !isfinite(n->valuedouble)) return false;
    output[i] = (float)n->valuedouble;
  }
  return true;
}
static bool json_float(const cJSON* root, const char* key, float* output) {
  const cJSON* n = cJSON_GetObjectItemCaseSensitive(root, key);
  if (!cJSON_IsNumber(n) || !isfinite(n->valuedouble)) return false;
  *output = (float)n->valuedouble;
  return true;
}
static esp_err_t controller_config_handler(httpd_req_t* req) {
  if (!authorized(req)) return ESP_OK;
  char body[1500];
  esp_err_t result = ESP_ERR_INVALID_ARG;
  cJSON* root = NULL;
  uint32_t config_id = 0;
  if (req->content_len > 0 && req->content_len < sizeof(body) &&
      receive_exact(req, body, req->content_len) == ESP_OK) {
    body[req->content_len] = 0;
    root = cJSON_ParseWithLengthOpts(body, req->content_len + 1, NULL, true);
    const char* keys[] = {"algorithm", "target", "parameters", "tolerance_ohm",
                          "max_duty"};
    unsigned seen = 0;
    bool valid = cJSON_IsObject(root);
    const cJSON* item;
    cJSON_ArrayForEach(item, root) {
      unsigned i;
      for (i = 0; i < 5; ++i)
        if (item->string && !strcmp(item->string, keys[i])) break;
      if (i == 5 || (seen & (1U << i))) {
        valid = false;
        break;
      }
      seen |= 1U << i;
    }
    controller_config_t config = {0};
    const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "algorithm");
    const cJSON* parameters =
        cJSON_GetObjectItemCaseSensitive(root, "parameters");
    if (valid && seen == 31 && cJSON_IsNumber(id) && id->valuedouble >= 1 &&
        id->valuedouble <= 255 && id->valuedouble == floor(id->valuedouble) &&
        cJSON_IsObject(parameters) &&
        json_four(root, "target", config.target) &&
        json_float(root, "tolerance_ohm", &config.tolerance_ohm) &&
        json_float(root, "max_duty", &config.max_duty)) {
      config.algorithm = (uint32_t)id->valuedouble;
      unsigned count;
      const controller_descriptor_t *registry =
                                        heater_controller_registry(&count),
                                    *descriptor = NULL;
      for (unsigned i = 0; i < count; ++i)
        if (registry[i].id == config.algorithm) descriptor = &registry[i];
      valid = descriptor && descriptor->parameter_count <= 16;
      seen = 0;
      cJSON_ArrayForEach(item, parameters) {
        if (!valid) break;
        unsigned i;
        for (i = 0; i < descriptor->parameter_count; ++i)
          if (item->string &&
              !strcmp(item->string, descriptor->parameters[i].name))
            break;
        if (i == descriptor->parameter_count || (seen & (1U << i))) {
          valid = false;
          break;
        }
        const controller_parameter_t* p = &descriptor->parameters[i];
        if (!p->count || p->offset + p->count > 32) {
          valid = false;
          break;
        }
        for (unsigned j = 0; j < p->count; ++j) {
          const cJSON* value =
              p->count == 1 ? item : cJSON_GetArrayItem(item, j);
          if ((p->count > 1 && (!cJSON_IsArray(item) ||
                                cJSON_GetArraySize(item) != (int)p->count)) ||
              !cJSON_IsNumber(value) || !isfinite(value->valuedouble) ||
              value->valuedouble < p->minimum ||
              value->valuedouble > p->maximum) {
            valid = false;
            break;
          }
          config.parameters[p->offset + j] = (float)value->valuedouble;
        }
        seen |= 1U << i;
      }
      if (valid && seen == ((1U << descriptor->parameter_count) - 1U))
        result = heater_service_configure(&config, &config_id);
    }
  }
  cJSON_Delete(root);
  if (result == ESP_OK) {
    char reply[128];
    snprintf(reply, sizeof(reply),
             "{\"ack\":\"CONFIGURED\",\"config_id\":%u,\"state\":\"preparing\","
             "\"running\":false}",
             (unsigned)config_id);
    return respond(req, "200 OK", reply);
  }
  return respond(
      req, result == ESP_ERR_INVALID_STATE ? "409 Conflict" : "400 Bad Request",
      "{\"error\":\"INVALID_CONFIG_OR_NOT_IDLE\"}");
}

// 鉴权并取得维护操作门闩，确认控制暂停后回复有效请求 `req` 并延时重启。
// 暂停失败释放门闩并回复冲突，确保无法确认关断时不进入重启流程。
static esp_err_t reboot_handler(httpd_req_t* req) {
  if (!authorized(req)) return ESP_OK;
  if (!device_operation_take(0))
    return respond(req, "409 Conflict", "{\"error\":\"BUSY\"}");
  const esp_err_t result = device_flash_begin(pdMS_TO_TICKS(2000));
  if (result != ESP_OK) {
    device_operation_give();
    return respond(req, "409 Conflict", "{\"error\":\"PAUSE_FAILED\"}");
  }
  respond(req, "200 OK", "{\"ack\":\"REBOOT\"}");
  vTaskDelay(pdMS_TO_TICKS(500));
  esp_restart();
  return ESP_OK;
}

// 保存单个 WebSocket 查看者的后续游标，仅在 HTTP 服务上下文访问。
// 会话由 HTTP 服务释放，重连后的持久游标由客户端重新提交。
typedef struct {
  uint64_t next;
} ws_session_t;

// 在升级握手前校验有效请求 `req` 的查询口令，并限制最多两个 WebSocket 查看者。
// 失败时回复 HTTP 错误并返回 ESP_FAIL，成功返回 ESP_OK。
static esp_err_t ws_auth(httpd_req_t* req) {
  char parameters[160], token[33];
  if (!query(req, parameters, sizeof(parameters)) ||
      !telemetry_form_value(parameters, "token", token, sizeof(token)) ||
      !device_credentials_authorized(token)) {
    respond(req, "401 Unauthorized", "{\"error\":\"AUTH_REQUIRED\"}");
    return ESP_FAIL;
  }
  size_t count = 8;
  int clients[8];
  unsigned websocket_count = 0;
  if (httpd_get_client_list(s_server, &count, clients) != ESP_OK)
    return ESP_FAIL;
  for (size_t i = 0; i < count; ++i)
    websocket_count +=
        httpd_ws_get_fd_info(s_server, clients[i]) == HTTPD_WS_CLIENT_WEBSOCKET;
  if (websocket_count >= WS_CLIENTS) {
    respond(req, "503 Service Unavailable", "{\"error\":\"TWO_VIEWERS_MAX\"}");
    return ESP_FAIL;
  }
  return ESP_OK;
}

// 在握手完成回调中为有效请求 `req` 分配独立游标，并设置 200 ms 发送超时。
// 会话由 HTTP 服务保存并用 free() 释放；启动 ID 改变时重置游标。
static esp_err_t ws_connected(httpd_req_t* req) {
  // IDF 6.1 升级握手不调用 URI 处理器，因此在专用完成回调初始化独立游标。
  ws_session_t* session = calloc(1, sizeof(*session));
  if (!session) return ESP_ERR_NO_MEM;
  char parameters[160], boot[32];
  if (!query(req, parameters, sizeof(parameters)) ||
      !cursor_from_query(parameters, &session->next)) {
    free(session);
    return ESP_FAIL;
  }
  if (telemetry_form_value(parameters, "boot", boot, sizeof(boot)) &&
      strcmp(boot, s_boot_id))
    session->next = 0;
  req->sess_ctx = session;
  req->free_ctx = free;
  // 限制慢查看者阻塞其他 HTTP 请求的时间。
  const struct timeval timeout = {.tv_usec = 200000};
  setsockopt(httpd_req_to_sockfd(req), SOL_SOCKET, SO_SNDTIMEO, &timeout,
             sizeof(timeout));
  return ESP_OK;
}

// 接收有效请求 `req` 上长度不超过 64 字节的 WebSocket 帧，不提交控制命令。
// 返回帧读取结果，控制命令经鉴权 HTTP 接口处理。
static esp_err_t ws_handler(httpd_req_t* req) {
  httpd_ws_frame_t frame = {0};
  if (httpd_ws_recv_frame(req, &frame, 0) != ESP_OK) return ESP_FAIL;
  // 控制命令通过鉴权 HTTP API 提交，此处仅接收查看连接的短帧。
  if (frame.len > 64) return ESP_FAIL;
  uint8_t input[65];
  frame.payload = input;
  return httpd_ws_recv_frame(req, &frame, frame.len);
}

// 在 HTTP 服务上下文逐个发送历史批次，仅在发送成功后推进各会话游标。
// 发送失败关闭会话，结束后清除待推送标记；`arg` 未使用。
static void push_work(void* arg) {
  (void)arg;
  size_t count = 8;
  int clients[8];
  if (httpd_get_client_list(s_server, &count, clients) == ESP_OK) {
    for (size_t i = 0; i < count; ++i) {
      if (httpd_ws_get_fd_info(s_server, clients[i]) !=
          HTTPD_WS_CLIENT_WEBSOCKET)
        continue;
      ws_session_t* session = httpd_sess_get_ctx(s_server, clients[i]);
      if (!session) continue;
      size_t length;
      uint64_t batch_next;
      char* payload = batch_json(session->next, WS_BATCH, &length, &batch_next);
      if (!payload) continue;
      httpd_ws_frame_t frame = {.type = HTTPD_WS_TYPE_TEXT,
                                .payload = (uint8_t*)payload,
                                .len = length};
      const esp_err_t sent =
          httpd_ws_send_frame_async(s_server, clients[i], &frame);
      free(payload);
      if (sent != ESP_OK) httpd_sess_trigger_close(s_server, clients[i]);
      // 重连时由客户端显式提供已持久化的游标。
      // socket 发送成功只表示传输已接受，不能证明上位机已经落盘。
      else
        session->next = batch_next;
    }
  }
  portENTER_CRITICAL(&s_lock);
  s_push_pending = false;
  portEXIT_CRITICAL(&s_lock);
}

// 按固定一秒周期向 HTTP 服务排入推送工作，待处理标记防止网络拥塞时重复积压。
// `arg` 未使用；web_push 位于 Core 0，优先级 3，栈为 4096 字节。
static void push_task(void* arg) {
  (void)arg;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    xTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));
    portENTER_CRITICAL(&s_lock);
    const bool queue = !s_push_pending;
    s_push_pending = true;
    portEXIT_CRITICAL(&s_lock);
    if (queue && httpd_queue_work(s_server, push_work, NULL) != ESP_OK) {
      portENTER_CRITICAL(&s_lock);
      s_push_pending = false;
      portEXIT_CRITICAL(&s_lock);
    }
  }
}

// 将非空的 HTTP 请求 `context` 适配为 OTA 读取回调，向 buffer 接收最多 size
// 字节。 返回实际字节数或 HTTP 接收错误，缓冲区由 OTA 模块提供。
static int ota_read(void* context, void* buffer, size_t size) {
  return httpd_req_recv(context, buffer, size);
}

// 在网页状态锁内更新 OTA 累计写入字节数 `received`，供状态接口显示进度。
// `context` 未使用，允许为 NULL。
static void ota_progress(void* context, size_t received) {
  (void)context;
  portENTER_CRITICAL(&s_lock);
  s_ota.received = received;
  portEXIT_CRITICAL(&s_lock);
}

// 接管非空的异步 HTTP 请求 `arg`，执行暂停、镜像写入与响应，然后完成异步请求。
// 成功延时重启；失败释放跨任务操作门闩并删除自身。
// ota_writer 位于 Core 0，优先级 5，栈为 6144 字节，缓冲使用内部 RAM。
static void ota_worker(void* arg) {
  httpd_req_t* req = arg;
  const esp_err_t result =
      ota_update_receive(req->content_len, ota_read, ota_progress, req,
                         s_control_init_result == ESP_OK);
  portENTER_CRITICAL(&s_lock);
  s_ota.error = result;
  s_ota.active = false;
  portEXIT_CRITICAL(&s_lock);
  if (result == ESP_OK)
    respond(req, "200 OK", "{\"ack\":\"OTA\",\"reboot\":true}");
  else
    respond(req, "400 Bad Request",
            "{\"error\":\"OTA_FAILED\",\"start_required\":true}");
  httpd_req_async_handler_complete(req);
  ESP_LOGI(TAG, "OTA result: %s; worker stack free: %u bytes",
           esp_err_to_name(result),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
  if (result == ESP_OK) {
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
  }
  device_operation_give();
  vTaskDelete(NULL);
}

// 鉴权并校验镜像大小及启动确认状态，将有效请求 `req` 转交专用 OTA 任务。
// 异步请求避免上传占用 HTTP 主循环；创建失败时释放请求与操作门闩。
static esp_err_t ota_handler(httpd_req_t* req) {
  if (!authorized(req)) return ESP_OK;
  const esp_partition_t* partition = esp_ota_get_next_update_partition(NULL);
  if (!partition || req->content_len < 512 ||
      req->content_len > partition->size)
    return respond(req, "400 Bad Request",
                   "{\"error\":\"INVALID_IMAGE_SIZE\"}");
  if (!device_maintenance_verified() || !device_operation_take(0))
    return respond(req, "409 Conflict",
                   "{\"error\":\"BUSY_OR_SELF_TEST_PENDING\"}");
  httpd_req_t* copy;
  if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
    device_operation_give();
    return ESP_FAIL;
  }
  portENTER_CRITICAL(&s_lock);
  s_ota.active = true;
  s_ota.total = req->content_len;
  s_ota.received = 0;
  s_ota.error = ESP_OK;
  portEXIT_CRITICAL(&s_lock);
  if (xTaskCreatePinnedToCore(ota_worker, "ota_writer", 6144, copy, 5, NULL,
                              0) != pdPASS) {
    respond(copy, "503 Service Unavailable", "{\"error\":\"NO_MEMORY\"}");
    httpd_req_async_handler_complete(copy);
    portENTER_CRITICAL(&s_lock);
    s_ota.active = false;
    portEXIT_CRITICAL(&s_lock);
    device_operation_give();
  }
  return ESP_OK;
}

// 注册页面、资源及 API；Core 0 HTTP 服务优先级 4，栈为 14336 字节。
esp_err_t device_web_start(esp_err_t control_init_result) {
  s_control_init_result = control_init_result;
  snprintf(s_boot_id, sizeof(s_boot_id), "%08" PRIx32 "%08" PRIx32,
           esp_random(), esp_random());
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.core_id = 0;
  config.task_priority = 4;
  config.stack_size = 14336;
  config.max_uri_handlers = 20;
  config.max_open_sockets = 6;
  config.recv_wait_timeout = 2;
  config.send_wait_timeout = 1;
  config.lru_purge_enable = true;
  esp_err_t result = httpd_start(&s_server, &config);
  if (result != ESP_OK) return result;
  const httpd_uri_t routes[] = {
      {.uri = "/", .method = HTTP_GET, .handler = index_handler},
      {.uri = "/dashboard.js", .method = HTTP_GET, .handler = asset_handler},
      {.uri = "/api.js", .method = HTTP_GET, .handler = asset_handler},
      {.uri = "/plot.js", .method = HTTP_GET, .handler = asset_handler},
      {.uri = "/stream.js", .method = HTTP_GET, .handler = asset_handler},
      {.uri = "/control.js", .method = HTTP_GET, .handler = asset_handler},
      {.uri = "/export.js", .method = HTTP_GET, .handler = asset_handler},
      {.uri = "/dashboard.css", .method = HTTP_GET, .handler = asset_handler},

      {.uri = "/api/experiments",
       .method = HTTP_GET,
       .handler = experiments_handler},
      {.uri = "/api/controllers",
       .method = HTTP_GET,
       .handler = controllers_handler},
      {.uri = "/api/controller/config",
       .method = HTTP_POST,
       .handler = controller_config_handler},
      {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
      {.uri = "/api/data", .method = HTTP_GET, .handler = data_handler},
      {.uri = "/api/control", .method = HTTP_POST, .handler = command_handler},
      {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler},
      {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_handler},
      {.uri = "/ws",
       .method = HTTP_GET,
       .handler = ws_handler,
       .is_websocket = true,
       .ws_pre_handshake_cb = ws_auth,
       .ws_post_handshake_cb = ws_connected},
  };
  for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
    result = httpd_register_uri_handler(s_server, &routes[i]);
    if (result != ESP_OK) return result;
  }
  if (xTaskCreatePinnedToCore(push_task, "web_push", 4096, NULL, 3, NULL, 0) !=
      pdPASS)
    return ESP_ERR_NO_MEM;
  ESP_LOGI(TAG,
           "HTTP dashboard ready on port 80; 1 Hz batches, up to 2 viewers");
  return ESP_OK;
}
