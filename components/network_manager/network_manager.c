#include "network_manager.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "heater_control.h"
#include "nvs.h"
#include "nvs_flash.h"

#define CONNECTED_BIT BIT0
#define AP_BIT BIT1

static const char *TAG = "network_manager";
static EventGroupHandle_t s_events;
static TaskHandle_t s_retry_task;
static esp_netif_t *s_sta;
static char s_token[33];
static char s_ap_ssid[32];
static bool s_configured;
static wifi_config_t s_ap_config;

static void stop_heat(void)
{
    const heater_command_t stop = {.kind = HEATER_COMMAND_STOP};
    heater_control_submit(&stop);
}

static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)data;
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "Dashboard: http://" IPSTR "/", IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_events, CONNECTED_BIT);
        if (s_retry_task) xTaskNotifyGive(s_retry_task);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, CONNECTED_BIT);
        stop_heat();
        if (s_retry_task) xTaskNotifyGive(s_retry_task);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        // A browser connection is not an independent guarantee of control ownership.
        stop_heat();
    }
}

esp_err_t network_manager_save(const char *ssid, const char *password)
{
    if (!heater_control_is_paused()) return ESP_ERR_INVALID_STATE;
    if (!ssid || !password || !*ssid || strlen(ssid) > 32 || strlen(password) > 64)
        return ESP_ERR_INVALID_ARG;
    const size_t password_size = strlen(password);
    if (password_size && password_size < 8) return ESP_ERR_INVALID_ARG;
    if (password_size == 64 && strspn(password, "0123456789abcdefABCDEF") != 64)
        return ESP_ERR_INVALID_ARG;
    // One NVS blob avoids a partially replaced SSID/password pair after power loss.
    struct { char ssid[33]; char password[65]; } settings = {0};
    strcpy(settings.ssid, ssid);
    strcpy(settings.password, password);
    nvs_handle_t handle;
    esp_err_t result = nvs_open("heater_net", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    result = nvs_set_blob(handle, "station", &settings, sizeof(settings));
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
    return result;
}

bool network_manager_authorized(const char *token)
{
    if (!token || strlen(token) != 32) return false;
    unsigned difference = 0;
    for (size_t i = 0; i < 32; ++i) difference |= (unsigned char)token[i] ^ (unsigned char)s_token[i];
    return difference == 0;
}

esp_err_t network_manager_init(void)
{
    // Do not silently erase NVS: it may contain calibration or credentials.
    esp_err_t result = nvs_flash_init();
    if (result != ESP_OK) return result;
    nvs_handle_t handle;
    result = nvs_open("heater_net", NVS_READWRITE, &handle);
    if (result != ESP_OK) return result;
    size_t size = sizeof(s_token);
    result = nvs_get_str(handle, "token", s_token, &size);
    if (result == ESP_ERR_NVS_NOT_FOUND) {
        uint8_t random[16];
        // RF is not started yet. Enable the SoC entropy source for provisioning.
        bootloader_random_enable();
        esp_fill_random(random, sizeof(random));
        bootloader_random_disable();
        for (size_t i = 0; i < sizeof(random); ++i) snprintf(s_token + i * 2, 3, "%02x", random[i]);
        result = nvs_set_str(handle, "token", s_token);
        if (result == ESP_OK) result = nvs_commit(handle);
    }
    if (result != ESP_OK || strlen(s_token) != 32) {
        nvs_close(handle);
        return result == ESP_OK ? ESP_ERR_INVALID_STATE : result;
    }
    struct { char ssid[33]; char password[65]; } settings = {0};
    size = sizeof(settings);
    result = nvs_get_blob(handle, "station", &settings, &size);
    nvs_close(handle);
    if (result != ESP_OK && result != ESP_ERR_NVS_NOT_FOUND) return result;
    s_configured = result == ESP_OK && size == sizeof(settings) && settings.ssid[0] &&
        memchr(settings.ssid, '\0', sizeof(settings.ssid)) &&
        memchr(settings.password, '\0', sizeof(settings.password));
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP));
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Heater-%02X%02X%02X", mac[3], mac[4], mac[5]);
    memcpy(s_ap_config.ap.ssid, s_ap_ssid, strlen(s_ap_ssid));
    s_ap_config.ap.ssid_len = strlen(s_ap_ssid);
    memcpy(s_ap_config.ap.password, s_token, 32);
    s_ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    s_ap_config.ap.max_connection = 4;
    s_ap_config.ap.channel = 1;
    ESP_LOGI(TAG, "Recovery AP: %s; AP password / API token: %s", s_ap_ssid, s_token);
    s_events = xEventGroupCreate();
    if (!s_events) return ESP_ERR_NO_MEM;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    if (!s_sta || !esp_netif_create_default_wifi_ap()) return ESP_ERR_NO_MEM;
    const wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL));
    ESP_ERROR_CHECK(esp_wifi_set_mode(s_configured ? WIFI_MODE_STA : WIFI_MODE_AP));
    if (s_configured) {
        wifi_config_t station = {0};
        memcpy(station.sta.ssid, settings.ssid, strlen(settings.ssid));
        memcpy(station.sta.password, settings.password, strlen(settings.password));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &station));
    } else {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &s_ap_config));
        xEventGroupSetBits(s_events, AP_BIT);
    }
    return ESP_OK;
}

static void retry_task(void *arg)
{
    (void)arg;
    unsigned failures = 0;
    for (;;) {
        const EventBits_t bits = xEventGroupGetBits(s_events);
        if (bits & CONNECTED_BIT) {
            failures = 0;
            if (bits & AP_BIT) {
                if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) xEventGroupClearBits(s_events, AP_BIT);
            }
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
            continue;
        }
        if (failures >= 5 && !(bits & AP_BIT)) {
            if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK &&
                esp_wifi_set_config(WIFI_IF_AP, &s_ap_config) == ESP_OK) {
                xEventGroupSetBits(s_events, AP_BIT);
                ESP_LOGW(TAG, "Router unavailable; recovery AP %s at 192.168.4.1", s_ap_ssid);
            }
        }
        const esp_err_t result = esp_wifi_connect();
        if (result != ESP_OK) ESP_LOGW(TAG, "Wi-Fi connect: %s", esp_err_to_name(result));
        const unsigned seconds = failures < 5 ? (1U << failures) : 30;
        // Disconnect events wake us, but reconnect attempts remain rate limited.
        vTaskDelay(pdMS_TO_TICKS(seconds * 1000));
        ulTaskNotifyTake(pdTRUE, 0);
        if (failures < 5) ++failures;
    }
}

esp_err_t network_manager_start(void)
{
    esp_err_t result = esp_wifi_start();
    if (result != ESP_OK) return result;
    ESP_ERROR_CHECK(esp_wifi_set_max_tx_power(CONFIG_ESP_PHY_MAX_WIFI_TX_POWER * 4));
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    if (s_configured && xTaskCreatePinnedToCore(retry_task, "wifi_reconnect", 4096, NULL, 4,
                                               &s_retry_task, 0) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

void network_manager_status(network_status_t *status)
{
    *status = (network_status_t){0};
    const EventBits_t bits = xEventGroupGetBits(s_events);
    status->connected = (bits & CONNECTED_BIT) != 0;
    status->ap_active = (bits & AP_BIT) != 0;
    strcpy(status->ap_ssid, s_ap_ssid);
    if (status->connected) {
        esp_netif_ip_info_t info;
        if (esp_netif_get_ip_info(s_sta, &info) == ESP_OK)
            snprintf(status->address, sizeof(status->address), IPSTR, IP2STR(&info.ip));
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) status->rssi = ap.rssi;
    } else if (status->ap_active) strcpy(status->address, "192.168.4.1");
}
