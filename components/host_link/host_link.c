#include "host_link.h"

#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "freertos/task.h"
#include "heater_control.h"
#include "heater_protocol.h"

#define HOST_TASK_STACK_BYTES 4096
#define HOST_RX_PRIORITY 4
#define HOST_TX_PRIORITY 3

static QueueHandle_t s_outbound;

static void reply(const char *text)
{
    heater_message_t message = {.kind = HEATER_MESSAGE_REPLY};
    snprintf(message.reply, sizeof(message.reply), "%s\n", text);
    // A host flood may lose an ACK; it cannot block the cycle task.
    xQueueSend(s_outbound, &message, 0);
}

static void process_command(const char *line, size_t length)
{
    heater_command_t command;
    if (!heater_command_parse(line, length, &command)) {
        reply("ERR,COMMAND");
        return;
    }
    const esp_err_t result = heater_control_submit(&command);
    if (result != ESP_OK) {
        reply("ERR,COMMAND");
    } else if (command.kind == HEATER_COMMAND_SET) {
        reply("ACK,SET");
    } else if (command.kind == HEATER_COMMAND_STOP) {
        reply("ACK,STOP");
    } else {
        reply("ACK,PING");
    }
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
                // Empty lines are ignored, so CRLF completes a command only once.
                if (discard) {
                    reply("ERR,LINE_TOO_LONG");
                } else if (used > 0) {
                    process_command(line, used);
                }
                used = 0;
                discard = false;
            } else if (!discard) {
                if (used == sizeof(line)) {
                    discard = true;
                } else {
                    line[used++] = c;
                }
            }
        }
    }
}

static void transmit_task(void *arg)
{
    (void)arg;
    heater_message_t message;
    for (;;) {
        if (xQueueReceive(s_outbound, &message, portMAX_DELAY) != pdPASS) {
            continue;
        }
        char line[HEATER_LINE_MAX_LENGTH];
        int length;
        if (message.kind == HEATER_MESSAGE_DATA) {
            length = heater_telemetry_format(&message.data, line, sizeof(line));
        } else {
            length = snprintf(line, sizeof(line), "%s", message.reply);
        }
        if (length <= 0 || (size_t)length >= sizeof(line)) {
            continue;
        }
        // Finish a whole line before taking the next message. A slow/disconnected
        // host blocks only this low-priority task; telemetry producers never wait.
        size_t sent = 0;
        while (sent < (size_t)length) {
            const int count = usb_serial_jtag_write_bytes(line + sent, length - sent,
                                                         pdMS_TO_TICKS(20));
            if (count > 0) {
                sent += (size_t)count;
            } else {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
    }
}

esp_err_t host_link_start(QueueHandle_t outbound)
{
    if (outbound == NULL || s_outbound != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = 2048,
        .rx_buffer_size = 512,
    };
    usb_serial_jtag_driver_config_t mutable_config = config;
    const esp_err_t result = usb_serial_jtag_driver_install(&mutable_config);
    if (result != ESP_OK) {
        return result;
    }
    s_outbound = outbound;
    TaskHandle_t tx_task;
    if (xTaskCreate(transmit_task, "host_tx", HOST_TASK_STACK_BYTES, NULL,
                    HOST_TX_PRIORITY, &tx_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(receive_task, "host_rx", HOST_TASK_STACK_BYTES, NULL,
                    HOST_RX_PRIORITY, NULL) != pdPASS) {
        vTaskDelete(tx_task);
        return ESP_ERR_NO_MEM;
    }
    reply("READY,1,20000,15000,660");
    return ESP_OK;
}
