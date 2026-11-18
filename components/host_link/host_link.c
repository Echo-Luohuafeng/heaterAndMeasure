#include "host_link.h"

#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heater_control.h"
#include "heater_protocol.h"
#include "telemetry_store.h"

static bool s_started;

// 以最多 20 ms 等待发送非空的 USB 回复 `text`，保持救援通道发送时间有界。
static void reply(const char *text)
{
    // USB 仅用于有界等待的救援/调试回复，不消费采样队列。
    usb_serial_jtag_write_bytes(text, strlen(text), pdMS_TO_TICKS(20));
}

// 阻塞接收 USB 字节，按回车/换行分帧；超长行丢弃到行尾，仅接受 STOP/PING。
// STOP 后等待缓存写入屏障再确认；`arg` 未使用。
// usb_rescue 位于 Core 0，优先级 4，栈为 4096 字节。
static void receive_task(void *arg)
{
    (void)arg;
    char line[HEATER_COMMAND_MAX_LENGTH];
    size_t used = 0;
    bool discard = false;
    for (;;) {
        uint8_t input[64];
        const int count = usb_serial_jtag_read_bytes(input, sizeof(input), pdMS_TO_TICKS(100));
        for (int i = 0; i < count; ++i) {
            const char c = (char)input[i];
            if (c == '\r' || c == '\n') {
                heater_command_t command;
                if (discard) reply("ERR,LINE_TOO_LONG\n");
                else if (used) {
                    if (!heater_command_parse(line, used, &command)) reply("ERR,COMMAND\n");
                    else if (command.kind == HEATER_COMMAND_SET || command.kind == HEATER_COMMAND_START) reply("ERR,USE_WIFI\n");
                    else if (heater_control_submit(&command) != ESP_OK) reply("ERR,MAINTENANCE\n");
                    else {
                        if (command.kind == HEATER_COMMAND_STOP) telemetry_store_sync_stop();
                        reply(command.kind == HEATER_COMMAND_STOP ? "ACK,STOP\n" : "ACK,PING\n");
                    }
                }
                used = 0; discard = false;
            } else if (!discard) {
                if (used == sizeof(line)) discard = true;
                else line[used++] = c;
            }
        }
    }
}

// 安装带收发缓冲的 USB 驱动，创建救援任务并发送就绪标记，拒绝重复启动。
esp_err_t host_link_start(void)
{
    if (s_started) return ESP_ERR_INVALID_STATE;
    usb_serial_jtag_driver_config_t config = {.tx_buffer_size = 1024, .rx_buffer_size = 512};
    esp_err_t result = usb_serial_jtag_driver_install(&config);
    if (result != ESP_OK) return result;
    if (xTaskCreatePinnedToCore(receive_task, "usb_rescue", 4096, NULL, 4, NULL, 0) != pdPASS)
        return ESP_ERR_NO_MEM;
    s_started = true;
    reply("READY,WIFI,50HZ,USB_STOP_ONLY\n");
    return ESP_OK;
}
