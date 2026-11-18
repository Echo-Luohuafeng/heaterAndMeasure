// 口令在启动阶段载入，运行期间只读取，不续约也不访问硬件输出。
#include "device_credentials.h"

#include <stdio.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"
static char s_token[33];
esp_err_t device_credentials_init(void) {
  esp_err_t result = nvs_flash_init();
  if (result != ESP_OK) return result;
  nvs_handle_t handle;
  result = nvs_open("heater_net", NVS_READWRITE, &handle);
  if (result != ESP_OK) return result;
  size_t size = sizeof(s_token);
  result = nvs_get_str(handle, "token", s_token, &size);
  if (result == ESP_ERR_NVS_NOT_FOUND) {
    uint8_t random[16];
    bootloader_random_enable();
    esp_fill_random(random, sizeof(random));
    bootloader_random_disable();
    for (size_t i = 0; i < sizeof(random); ++i)
      snprintf(s_token + i * 2, 3, "%02x", random[i]);
    result = nvs_set_str(handle, "token", s_token);
    if (result == ESP_OK) result = nvs_commit(handle);
  }
  nvs_close(handle);
  if (result != ESP_OK) return result;
  if (strlen(s_token) != 32) return ESP_ERR_INVALID_STATE;
  return ESP_OK;
}
bool device_credentials_authorized(const char* token) {
  if (!token || strlen(token) != 32) return false;
  unsigned difference = 0;
  for (size_t i = 0; i < 32; ++i)
    difference |= (unsigned char)token[i] ^ (unsigned char)s_token[i];
  return difference == 0;
}

void device_credentials_log(const char* address) {
  ESP_LOGI("device_credentials",
           "USB CDC-NCM dashboard: http://%s/; API token: %s", address,
           s_token);
}
