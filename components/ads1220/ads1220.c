#include "ads1220.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "heater_board.h"
#include "heater_protocol.h"

#define ADC_SPI_HOST SPI2_HOST
#define ADC_SPI_CLOCK_HZ 1000000
#define ADC_RESET 0x06
#define ADC_START_SYNC 0x08
#define ADC_RDATA 0x10
#define ADC_RREG 0x20
#define ADC_WREG 0x40

static const char *TAG = "ads1220";
// The S3 SPI host has only three hardware CS slots. Use one SPI device and four
// software CS GPIOs so all four converters share the same bus configuration.
static spi_device_handle_t s_device;
// AIN0-AIN1, PGA gain 4; Turbo 660 SPS single-shot; REFP0/REFN0,
// 1.5 mA IDAC; IDAC1 routed to AIN2, IDAC2 off; dedicated DRDY only.
static const uint8_t s_config[4] = {0x04, 0x90, 0x47, 0x60};

static esp_err_t transfer(size_t channel, const uint8_t *tx, uint8_t *rx, size_t length)
{
    if (channel >= HEATER_CHANNEL_COUNT || s_device == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    spi_transaction_t transaction = {
        .length = length * 8,
        .tx_buffer = tx,
        .rx_buffer = rx,
    };
    // Polling is confined to the control task. No DMA or long bus queue is needed
    // for 1-6 byte transactions on this exclusively owned bus.
    gpio_set_level(heater_adc_cs_gpio[channel], 0);
    const esp_err_t result = spi_device_polling_transmit(s_device, &transaction);
    gpio_set_level(heater_adc_cs_gpio[channel], 1);
    return result;
}

esp_err_t ads1220_init(void)
{
    uint64_t cs_mask = 0;
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
        cs_mask |= 1ULL << heater_adc_cs_gpio[i];
        gpio_set_level(heater_adc_cs_gpio[i], 1);
    }
    const gpio_config_t cs_config = {.pin_bit_mask = cs_mask, .mode = GPIO_MODE_OUTPUT};
    ESP_RETURN_ON_ERROR(gpio_config(&cs_config), TAG, "ADC CS GPIO failed");
    const spi_bus_config_t bus = {
        .mosi_io_num = HEATER_SPI_MOSI_GPIO,
        .miso_io_num = HEATER_SPI_MISO_GPIO,
        .sclk_io_num = HEATER_SPI_SCK_GPIO,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 8,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(ADC_SPI_HOST, &bus, SPI_DMA_DISABLED),
                        TAG, "SPI bus initialization failed");
    const spi_device_interface_config_t device = {
        .clock_speed_hz = ADC_SPI_CLOCK_HZ,
        .mode = 1,
        .spics_io_num = -1,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(spi_bus_add_device(ADC_SPI_HOST, &device, &s_device),
                        TAG, "ADC SPI device failed");
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
        const uint8_t reset = ADC_RESET;
        ESP_RETURN_ON_ERROR(transfer(i, &reset, NULL, 1), TAG, "ADC reset failed");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
        // Program the IDAC current before connecting its output.
        const uint8_t initial[] = {ADC_WREG | 3, s_config[0], s_config[1], s_config[2], 0};
        ESP_RETURN_ON_ERROR(transfer(i, initial, NULL, sizeof(initial)), TAG, "ADC configuration failed");
        esp_rom_delay_us(200);
        const uint8_t route[] = {ADC_WREG | (3 << 2), s_config[3]};
        ESP_RETURN_ON_ERROR(transfer(i, route, NULL, sizeof(route)), TAG, "IDAC routing failed");
        const uint8_t read[] = {ADC_RREG | 3, 0, 0, 0, 0};
        uint8_t reply[sizeof(read)] = {0};
        ESP_RETURN_ON_ERROR(transfer(i, read, reply, sizeof(read)), TAG, "ADC verification failed");
        if (memcmp(reply + 1, s_config, sizeof(s_config)) != 0) {
            ESP_LOGE(TAG, "ADC %u registers: %02x %02x %02x %02x; expected 04 90 47 60",
                     (unsigned)i + 1, reply[1], reply[2], reply[3], reply[4]);
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    // WREG starts an initial conversion; its result is discarded. Do not send
    // POWERDOWN: the IDACs must remain biased while PhotoMOS isolates heating.
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

esp_err_t ads1220_start(size_t channel)
{
    const uint8_t start = ADC_START_SYNC;
    return transfer(channel, &start, NULL, 1);
}

esp_err_t ads1220_read(size_t channel, int32_t *code)
{
    if (code == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint8_t tx[] = {ADC_RDATA, 0, 0, 0};
    uint8_t rx[sizeof(tx)];
    const esp_err_t result = transfer(channel, tx, rx, sizeof(tx));
    if (result == ESP_OK) {
        *code = heater_decode_adc24(rx + 1);
    }
    return result;
}
