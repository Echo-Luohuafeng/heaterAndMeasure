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
#define ADC_SPI_CLOCK_HZ 2000000
#define ADC_RESET 0x06
#define ADC_START_SYNC 0x08
#define ADC_RDATA 0x10
#define ADC_RREG 0x20
#define ADC_WREG 0x40

static const char *TAG = "ads1220";
// S3 SPI 主机只有三个硬件片选槽，使用单个设备句柄与四路软件片选。
// 四个 ADC 共用总线配置，所有事务须串行执行。
static spi_device_handle_t s_device;
// AIN0−AIN1、PGA 增益 4、Turbo 660 SPS 单次转换、REFP0/REFN0 外部参考。
// IDAC1 以 1.5 mA 激励 AIN2，IDAC2 关闭，使用独立 DRDY 引脚。
static const uint8_t s_config[4] = {0x04, 0x90, 0x47, 0x60};

// 用软件片选选中 `channel`，在独占 SPI 总线上执行短事务并恢复片选为高。
// `tx` 须提供 length 字节，`rx` 可为 NULL；四字节以内使用事务内联缓冲。
// 仅允许初始化流程或控制任务串行调用，返回参数检查或 SPI 传输结果。
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
    if (length <= sizeof(transaction.tx_data)) {
        transaction.flags |= SPI_TRANS_USE_TXDATA;
        memcpy(transaction.tx_data, tx, length);
        if (rx) transaction.flags |= SPI_TRANS_USE_RXDATA;
    }
    // 初始化后仅由控制任务轮询传输；独占总线上的短事务无需 DMA 或长队列。
    gpio_set_level(heater_adc_cs_gpio[channel], 0);
    const esp_err_t result = spi_device_polling_transmit(s_device, &transaction);
    gpio_set_level(heater_adc_cs_gpio[channel], 1);
    if (result == ESP_OK && rx && (transaction.flags & SPI_TRANS_USE_RXDATA))
        memcpy(rx, transaction.rx_data, length);
    return result;
}

// 建立单个 SPI 设备及四路软件片选，复位后先配置 IDAC 电流，再接通路由。
// 逐路读回寄存器验证配置，等待并丢弃初始转换，保持激励供后续周期使用。
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
    // 初始化与控制均使用该设备句柄独占总线，长期持有设备级总线锁以减少逐次仲裁。
    ESP_RETURN_ON_ERROR(spi_device_acquire_bus(s_device, portMAX_DELAY), TAG, "SPI bus acquisition failed");
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
        const uint8_t reset = ADC_RESET;
        ESP_RETURN_ON_ERROR(transfer(i, &reset, NULL, 1), TAG, "ADC reset failed");
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
        // 先配置 IDAC 电流，再连接输出，避免路由接通时激励尚未配置。
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
    // WREG 触发的初始转换不参与采样；PhotoMOS 隔离加热期间 IDAC 仍需保持激励。
    // 因此周期间不发送会关闭激励的 POWERDOWN。
    vTaskDelay(pdMS_TO_TICKS(10));
    return ESP_OK;
}

// 向选定通道发送 START/SYNC 命令，复用独占总线的短事务路径。
esp_err_t ads1220_start(size_t channel)
{
    const uint8_t start = ADC_START_SYNC;
    return transfer(channel, &start, NULL, 1);
}

// 发送 RDATA 并收取三字节大端补码，仅在传输成功时写回符号扩展结果。
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
