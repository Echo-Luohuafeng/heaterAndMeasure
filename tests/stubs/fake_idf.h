#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_INVALID_STATE 2
#define ESP_ERR_NO_MEM 3
#define ESP_ERR_TIMEOUT 4
#define ESP_ERR_NOT_FOUND 5
#define ESP_FAIL -1
#define IRAM_ATTR
#define DRAM_ATTR
#define ESP_LOGI(tag, ...) ((void)(tag))
#define ESP_RETURN_ON_ERROR(expression, tag, ...) do { \
    (void)(tag); esp_err_t fake_result = (expression); \
    if (fake_result != ESP_OK) return fake_result; \
} while (0)
typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef unsigned TickType_t;
typedef void *TaskHandle_t;
typedef void *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
#define portENTER_CRITICAL_ISR(lock) ((void)(lock))
#define portEXIT_CRITICAL_ISR(lock) ((void)(lock))
#define portYIELD_FROM_ISR() ((void)0)
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
#define eSetBits 0
#define pdMS_TO_TICKS(ms) ((ms) / 10)
BaseType_t xTaskCreate(void (*entry)(void *), const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *);
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *, uint32_t, void *, UBaseType_t, TaskHandle_t *, BaseType_t);
BaseType_t xTaskNotify(TaskHandle_t, uint32_t, int);
BaseType_t xTaskNotifyFromISR(TaskHandle_t, uint32_t, int, BaseType_t *);
BaseType_t xTaskNotifyWait(uint32_t, uint32_t, uint32_t *, TickType_t);
BaseType_t xQueueSend(QueueHandle_t, const void *, TickType_t);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t);
void vTaskDelay(TickType_t);

typedef void *gptimer_handle_t;
typedef struct { uint64_t alarm_value; uint64_t count_value; } gptimer_alarm_event_data_t;
typedef struct {
    int clk_src, direction;
    uint32_t resolution_hz;
    int intr_priority;
} gptimer_config_t;
typedef struct { uint64_t alarm_count; uint64_t reload_count; } gptimer_alarm_config_t;
typedef struct {
    bool (*on_alarm)(gptimer_handle_t, const gptimer_alarm_event_data_t *, void *);
} gptimer_event_callbacks_t;
#define GPTIMER_CLK_SRC_DEFAULT 0
#define GPTIMER_COUNT_UP 0
esp_err_t gptimer_new_timer(const gptimer_config_t *, gptimer_handle_t *);
esp_err_t gptimer_register_event_callbacks(gptimer_handle_t, const gptimer_event_callbacks_t *, void *);
esp_err_t gptimer_set_alarm_action(gptimer_handle_t, const gptimer_alarm_config_t *);
esp_err_t gptimer_get_raw_count(gptimer_handle_t, uint64_t *);
esp_err_t gptimer_set_raw_count(gptimer_handle_t, uint64_t);
int64_t esp_timer_get_time(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t, TickType_t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t);
esp_err_t gptimer_enable(gptimer_handle_t);
esp_err_t gptimer_start(gptimer_handle_t);
esp_err_t gptimer_stop(gptimer_handle_t);
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, intr_type;
} gpio_config_t;
#define GPIO_MODE_INPUT 0
#define GPIO_PULLUP_ENABLE 1
#define GPIO_INTR_NEGEDGE 2
#define ESP_INTR_FLAG_IRAM 1
#define ESP_INTR_FLAG_LEVEL3 2
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_install_isr_service(int);
esp_err_t gpio_isr_handler_add(int, void (*)(void *), void *);

QueueHandle_t xQueueCreate(UBaseType_t, UBaseType_t);
BaseType_t xQueueReceive(QueueHandle_t, void *, TickType_t);
BaseType_t xQueueOverwrite(QueueHandle_t, const void *);
