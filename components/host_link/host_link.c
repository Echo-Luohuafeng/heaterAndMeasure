#include "host_link.h"

#include <string.h>
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heater_control.h"
#include "heater_protocol.h"

static bool s_started;

static void reply(const char *text)
{
    // USB is a bounded emergency/debug channel; it never consumes samples.
    usb_serial_jtag_write_bytes(text, strlen(text), pdMS_TO_TICKS(20));
}

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
                    else if (command.kind == HEATER_COMMAND_SET) reply("ERR,USE_WIFI\n");
                    else if (heater_control_submit(&command) != ESP_OK) reply("ERR,MAINTENANCE\n");
                    else reply(command.kind == HEATER_COMMAND_STOP ? "ACK,STOP\n" : "ACK,PING\n");
                }
                used = 0; discard = false;
            } else if (!discard) {
                if (used == sizeof(line)) discard = true;
                else line[used++] = c;
            }
        }
    }
}

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
