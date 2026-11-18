#include "network_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootloader_random.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "heater_control.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "tinyusb.h"
#include "tinyusb_default_config.h"
#include "device/usbd_pvt.h"

#define USB_ADDRESS "192.168.7.1"
#define TX_QUEUE_LENGTH 8
#define FRAME_BYTES 1514

static const char *TAG = "network_manager";
static esp_netif_t *s_netif;
static TaskHandle_t s_link_task;
static QueueHandle_t s_tx_queue;
static char s_token[33];
static char s_serial[13];
static char s_host_mac_string[13];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_connected;
static bool s_pump_pending;
static bool s_started;
static uint32_t s_rx_drops, s_tx_drops;
static uint32_t s_link_generation;

// USB 主机网卡 MAC 与设备 lwIP MAC 不同，避免 ARP 将自身当成对端。
static const char *s_strings[] = {
    (const char[]){0x09, 0x04}, "Espressif", "Heater USB network",
    s_serial, "Heater rescue console", "Heater CDC-NCM", s_host_mac_string,
};
static const uint8_t s_config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 4, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN +
                          TUD_CDC_NCM_DESC_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64),
    TUD_CDC_NCM_DESCRIPTOR(2, 5, 6, 0x83, 64, 0x04, 0x84, 64, FRAME_BYTES),
};

typedef struct {
    uint32_t generation;
    uint16_t length;
    uint8_t bytes[FRAME_BYTES];
} usb_frame_t;
// 固定内部 RAM 缓冲，所有 tud_network_* 收发操作在 TinyUSB 任务中完成。
static usb_frame_t s_tx_frame;

static void stop_heat(void)
{
    const heater_command_t stop = {.kind = HEATER_COMMAND_STOP};
    heater_control_submit(&stop);
}

static void set_connected(bool connected)
{
    portENTER_CRITICAL(&s_lock);
    s_connected = connected;
    if (!connected) ++s_link_generation;
    portEXIT_CRITICAL(&s_lock);
    // STOP 不阻塞、不写 Flash；事件回调立即取消当前运行。
    if (!connected) stop_heat();
    if (s_link_task) xTaskNotifyGive(s_link_task);
}

static void usb_event(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    set_connected(event->id == TINYUSB_EVENT_ATTACHED);
}

// USB 挂起时也撤销控制；恢复或重新枚举不会重新 START。
void tud_suspend_cb(bool remote_wakeup_en)
{
    (void)remote_wakeup_en;
    set_connected(false);
}

void tud_resume_cb(void)
{
    set_connected(tud_mounted());
}

bool network_manager_authorized(const char *token)
{
    if (!token || strlen(token) != 32) return false;
    unsigned difference = 0;
    for (size_t i = 0; i < 32; ++i)
        difference |= (unsigned char)token[i] ^ (unsigned char)s_token[i];
    return difference == 0;
}

void network_manager_log_connection(void)
{
    ESP_LOGI(TAG, "USB CDC-NCM dashboard: http://" USB_ADDRESS "/; API token: %s", s_token);
}

static void free_rx(void *handle, void *buffer)
{
    (void)handle;
    free(buffer);
}

// TinyUSB 的 RX 缓冲在回调后立即复用，必须复制后才交给异步 lwIP。
// ethernetif_input 无论成功失败均接管副本，不能再次释放。
bool tud_network_recv_cb(const uint8_t *buffer, uint16_t length)
{
    if (!s_netif || !length || length > FRAME_BYTES) {
        tud_network_recv_renew();
        return true;
    }
    void *copy = heap_caps_malloc(length, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (copy) {
        memcpy(copy, buffer, length);
        if (esp_netif_receive(s_netif, copy, length, NULL) == ESP_OK) {
            tud_network_recv_renew();
            return true;
        }
    }
    portENTER_CRITICAL(&s_lock);
    ++s_rx_drops;
    portEXIT_CRITICAL(&s_lock);
    tud_network_recv_renew();
    return true;
}

uint16_t tud_network_xmit_cb(uint8_t *destination, void *reference, uint16_t length)
{
    const usb_frame_t *frame = reference;
    memcpy(destination, frame->bytes, length);
    return length;
}

void tud_network_init_cb(void)
{
    // NCM 由主机选择数据接口后自行开始接收；设备无无线桥接依赖。
}

// 在 TinyUSB 上下文中消费有界队列。NTB 满时留给下一次调度，避免忙等。
static void pump_tx(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < TX_QUEUE_LENGTH; ++i) {
        if (xQueuePeek(s_tx_queue, &s_tx_frame, 0) != pdTRUE) break;
        portENTER_CRITICAL(&s_lock);
        const bool stale = s_tx_frame.generation != s_link_generation;
        portEXIT_CRITICAL(&s_lock);
        if (!tud_ready() || stale) {
            if (xQueueReceive(s_tx_queue, &s_tx_frame, 0) != pdTRUE) break;
            portENTER_CRITICAL(&s_lock);
            ++s_tx_drops;
            portEXIT_CRITICAL(&s_lock);
        } else {
            if (!tud_network_can_xmit(s_tx_frame.length)) break;
            if (xQueueReceive(s_tx_queue, &s_tx_frame, 0) != pdTRUE) break;
            tud_network_xmit(&s_tx_frame, s_tx_frame.length);
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_pump_pending = false;
    portEXIT_CRITICAL(&s_lock);
}

// lwIP 输出只复制到固定长度内部 RAM 队列，不等待 USB，拥塞由 TCP 重传处理。
static esp_err_t transmit(void *handle, void *buffer, size_t length)
{
    (void)handle;
    if (!length || length > FRAME_BYTES) return ESP_ERR_INVALID_SIZE;
    portENTER_CRITICAL(&s_lock);
    const bool connected = s_connected;
    const uint32_t generation = s_link_generation;
    portEXIT_CRITICAL(&s_lock);
    if (!connected) return ESP_ERR_INVALID_STATE;
    // esp-netif Ethernet 输出只由 lwIP TCP/IP 任务调用；静态暂存避免占用其栈。
    static usb_frame_t frame;
    frame.length = length;
    frame.generation = generation;
    memcpy(frame.bytes, buffer, length);
    if (xQueueSend(s_tx_queue, &frame, 0) != pdTRUE) {
        portENTER_CRITICAL(&s_lock);
        ++s_tx_drops;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NO_MEM;
    }
    if (s_link_task) xTaskNotifyGive(s_link_task);
    return ESP_OK;
}

// Core 0、优先级 4、4096 字节；每 10 ms 或通知后安排一次 USB 发送。
// 最多一个 deferred pump，避免拥塞时堆积 USB 工作事件。
static void link_task(void *arg)
{
    (void)arg;
    for (;;) {
        // 空队列阻塞等待；仅在 USB NTB 拥塞时每 10 ms 重试。
        ulTaskNotifyTake(pdTRUE, uxQueueMessagesWaiting(s_tx_queue) ? pdMS_TO_TICKS(10) : portMAX_DELAY);
        if (!uxQueueMessagesWaiting(s_tx_queue)) continue;
        portENTER_CRITICAL(&s_lock);
        const bool schedule = !s_pump_pending;
        if (schedule) s_pump_pending = true;
        portEXIT_CRITICAL(&s_lock);
        if (schedule) usbd_defer_func(pump_tx, NULL, false);
    }
}

// 保留原 NVS token；首次生成与写入在采样启动前完成，不擦除已有配置。
esp_err_t network_manager_init(void)
{
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
    result = esp_netif_init();
    if (result != ESP_OK) return result;
    result = esp_event_loop_create_default();
    if (result != ESP_OK) return result;
    s_tx_queue = xQueueCreate(TX_QUEUE_LENGTH, sizeof(usb_frame_t));
    if (!s_tx_queue) return ESP_ERR_NO_MEM;

    uint8_t mac[6];
    result = esp_efuse_mac_get_default(mac);
    if (result != ESP_OK) return result;
    for (size_t i = 0; i < sizeof(mac); ++i)
        snprintf(s_serial + i * 2, 3, "%02X", mac[i]);
    mac[0] = (mac[0] | 0x02) & 0xfe;
    for (size_t i = 0; i < sizeof(mac); ++i)
        snprintf(s_host_mac_string + i * 2, 3, "%02X", mac[i]);
    mac[5] ^= 0x01;
    esp_netif_ip_info_t ip = {0};
    esp_netif_str_to_ip4(USB_ADDRESS, &ip.ip);
    esp_netif_str_to_ip4("255.255.255.0", &ip.netmask);
    const esp_netif_inherent_config_t base = {
        .flags = ESP_NETIF_DHCP_SERVER | ESP_NETIF_FLAG_AUTOUP,
        .ip_info = &ip, .if_key = "USB_NCM", .if_desc = "usb", .route_prio = 10,
    };
    const esp_netif_driver_ifconfig_t driver = {
        .handle = s_tx_queue, .transmit = transmit, .driver_free_rx_buffer = free_rx,
    };
    const esp_netif_config_t config = {
        .base = &base, .driver = &driver, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH,
    };
    s_netif = esp_netif_new(&config);
    if (!s_netif) return ESP_ERR_NO_MEM;
    result = esp_netif_set_mac(s_netif, mac);
    if (result != ESP_OK) return result;
    // 不向电脑发布默认路由或 DNS，保留电脑已有的互联网连接。
    uint8_t offer = 0;
    result = esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                                   ESP_NETIF_ROUTER_SOLICITATION_ADDRESS, &offer, sizeof(offer));
    if (result != ESP_OK) return result;
    result = esp_netif_dhcps_option(s_netif, ESP_NETIF_OP_SET,
                                   ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    return result;
}

esp_err_t network_manager_start(void)
{
    if (!s_netif || s_started) return ESP_ERR_INVALID_STATE;
    // 网络先准备好；USB 回调不调用同步 netif 操作，避免 USB/lwIP 相互等待。
    esp_netif_action_start(s_netif, NULL, 0, NULL);
    esp_netif_dhcp_status_t dhcp;
    esp_err_t result = esp_netif_dhcps_get_status(s_netif, &dhcp);
    if (result != ESP_OK) return result;
    if (dhcp != ESP_NETIF_DHCP_STARTED) return ESP_FAIL;
    // driver_install 内部启动任务保存 descriptor 指针，配置必须具有静态生命周期。
    static tinyusb_config_t usb;
    usb = TINYUSB_DEFAULT_CONFIG(usb_event);
    usb.task = TINYUSB_TASK_CUSTOM(4096, 5, 0);
    usb.descriptor.string = s_strings;
    usb.descriptor.string_count = sizeof(s_strings) / sizeof(s_strings[0]);
    usb.descriptor.full_speed_config = s_config_descriptor;
    result = tinyusb_driver_install(&usb);
    if (result != ESP_OK) return result;
    if (xTaskCreatePinnedToCore(link_task, "usb_network", 4096, NULL, 4,
                               &s_link_task, 0) != pdPASS) return ESP_ERR_NO_MEM;
    s_started = true;
    return ESP_OK;
}

void network_manager_status(network_status_t *status)
{
    *status = (network_status_t){0};
    strcpy(status->address, USB_ADDRESS);
    portENTER_CRITICAL(&s_lock);
    status->connected = s_connected;
    status->rx_drops = s_rx_drops;
    status->tx_drops = s_tx_drops;
    portEXIT_CRITICAL(&s_lock);
    TaskHandle_t usb_task = xTaskGetHandle("TinyUSB");
    if (usb_task) status->usb_stack_free_bytes = uxTaskGetStackHighWaterMark(usb_task);
    if (s_link_task) status->network_stack_free_bytes = uxTaskGetStackHighWaterMark(s_link_task);
}
