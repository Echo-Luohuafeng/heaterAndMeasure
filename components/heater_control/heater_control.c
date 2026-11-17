#include "heater_control.h"

#include <math.h>
#include <string.h>

#include "ads1220.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "freertos/task.h"
#include "heater_board.h"
#include "sdkconfig.h"

#if !CONFIG_GPTIMER_ISR_CACHE_SAFE || !CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM || \
    !CONFIG_MCPWM_CTRL_FUNC_IN_IRAM || !CONFIG_GPIO_CTRL_FUNC_IN_IRAM
#error "Heater phase callbacks require cache-safe GPTimer, MCPWM and GPIO control functions"
#endif

#define CONTROL_TASK_STACK_BYTES 6144
#define CONTROL_TASK_PRIORITY 20
#define ADC_START_US 17000U
// Four software-CS START commands exceed 100 us on the S3 at 160 MHz.
// Complete them by 17.2 ms, leaving 1.85 ms before hardware measurement cutoff.
#define ADC_START_DEADLINE_US 17200U
#define MEASURE_OFF_US 19050U
#define ADC_READ_GUARD_US 100U
#define ADC_EARLIEST_READY_US 1200U
#define ALARM_REARM_GUARD_US 20U
#define ALL_CHANNELS_MASK ((1U << HEATER_CHANNEL_COUNT) - 1U)
#define EVENT_FRAME_START (1U << 0)
#define EVENT_CONVERT (1U << 1)
#define EVENT_ADC_READY (1U << 2)
#define EVENT_FRAME_DONE (1U << 3)
#define EVENT_ABORT (1U << 4)

typedef enum {
    PHASE_START,
    PHASE_DISCHARGE,
    PHASE_ISOLATE,
    PHASE_MEASURE_ON,
    PHASE_CONVERT,
    PHASE_MEASURE_OFF,
    PHASE_DONE,
    PHASE_PRECHARGE,
    PHASE_COUNT,
    PHASE_ABORT,
} phase_t;

// Writable DRAM: an IRAM ISR must not read a flash-resident const table.
static DRAM_ATTR uint32_t s_phase_us[PHASE_COUNT] = {
    0, 15000, 15100, 15200, ADC_START_US, MEASURE_OFF_US, 19350, 19900,
};
static uint8_t s_drdy_channel[HEATER_CHANNEL_COUNT] = {0, 1, 2, 3};
static const uint32_t s_reference_mohm[HEATER_CHANNEL_COUNT] = {
    CONFIG_HEATER_REFERENCE_1_MOHM, CONFIG_HEATER_REFERENCE_2_MOHM,
    CONFIG_HEATER_REFERENCE_3_MOHM, CONFIG_HEATER_REFERENCE_4_MOHM,
};
static const int32_t s_offset_mohm[HEATER_CHANNEL_COUNT] = {
    CONFIG_HEATER_OFFSET_1_MOHM, CONFIG_HEATER_OFFSET_2_MOHM,
    CONFIG_HEATER_OFFSET_3_MOHM, CONFIG_HEATER_OFFSET_4_MOHM,
};
static const char *TAG = "heater_control";

typedef struct {
    portMUX_TYPE lock;
    gptimer_handle_t timer;
    TaskHandle_t task;
    QueueHandle_t outbound;
    gptimer_alarm_config_t alarm;
    phase_t next_phase;
    phase_t phase;
    uint64_t cycle_start_us;
    uint64_t next_cycle_start_us;
    uint32_t sequence;
    uint8_t desired[HEATER_CHANNEL_COUNT];
    uint8_t applied[HEATER_CHANNEL_COUNT];
    bool command_active;
    bool bootstrap_ready;
    bool pwm_failed;
    uint8_t healthy_mask;
    uint32_t healthy_sequence;
    uint8_t started_mask;
    uint8_t ready_mask;
    uint64_t adc_start_us[HEATER_CHANNEL_COUNT];
    uint32_t faults;
    uint32_t deadline_misses;
    uint32_t telemetry_drops;
} control_t;

static control_t s_control = {.lock = portMUX_INITIALIZER_UNLOCKED};

typedef struct {
    phase_t phase;
    uint32_t sequence;
    uint64_t cycle_start_us;
    uint8_t duty[HEATER_CHANNEL_COUNT];
    uint8_t ready_mask;
    uint32_t faults;
} snapshot_t;

static snapshot_t snapshot(void)
{
    snapshot_t result;
    portENTER_CRITICAL(&s_control.lock);
    result.phase = s_control.phase;
    result.sequence = s_control.sequence;
    result.cycle_start_us = s_control.cycle_start_us;
    memcpy(result.duty, s_control.applied, sizeof(result.duty));
    result.ready_mask = s_control.ready_mask;
    result.faults = s_control.faults;
    portEXIT_CRITICAL(&s_control.lock);
    return result;
}

static void add_fault(uint32_t sequence, uint32_t fault)
{
    portENTER_CRITICAL(&s_control.lock);
    if (s_control.sequence == sequence) {
        s_control.faults |= fault;
    }
    portEXIT_CRITICAL(&s_control.lock);
}

static void IRAM_ATTR pwm_failure(void)
{
    heater_board_requests_off();
    s_control.pwm_failed = true;
    s_control.bootstrap_ready = false;
    s_control.faults |= HEATER_FAULT_PWM;
}

static bool IRAM_ATTR phase_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *event,
                                 void *user_data)
{
    (void)user_data;
    uint32_t notification = 0;
    BaseType_t woken = pdFALSE;
    portENTER_CRITICAL_ISR(&s_control.lock);
    const phase_t phase = s_control.next_phase;
    s_control.phase = phase;
    if (event->count_value > event->alarm_value + CONFIG_HEATER_MAX_ISR_LATENCY_US) {
        // Recover at a future absolute frame boundary. Never re-arm the past.
        heater_board_requests_off();
        heater_board_pwm_low();
        s_control.bootstrap_ready = false;
        s_control.started_mask = 0;
        s_control.healthy_mask = 0;
        s_control.faults |= HEATER_FAULT_TIMING;
        ++s_control.deadline_misses;
        s_control.phase = PHASE_ABORT;
        s_control.next_phase = PHASE_START;
        s_control.next_cycle_start_us = (event->count_value / HEATER_PERIOD_US + 1) * HEATER_PERIOD_US;
        s_control.alarm.alarm_count = s_control.next_cycle_start_us;
        notification = EVENT_ABORT;
    } else {
        switch (phase) {
        case PHASE_START: {
            s_control.cycle_start_us = s_control.next_cycle_start_us;
            ++s_control.sequence;
            s_control.started_mask = 0;
            s_control.ready_mask = 0;
            s_control.faults = s_control.pwm_failed ? HEATER_FAULT_PWM : 0;
            const bool previous_done = s_control.healthy_sequence == s_control.sequence - 1;
            if (!previous_done) {
                s_control.faults |= HEATER_FAULT_TIMING;
                ++s_control.deadline_misses;
            }
            bool heat = false;
            for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
                const bool allowed = previous_done && s_control.bootstrap_ready &&
                    !s_control.pwm_failed && s_control.command_active &&
                    (s_control.healthy_mask & (1U << i));
                s_control.applied[i] = allowed ? s_control.desired[i] : 0;
                heat |= s_control.applied[i] != 0;
            }
            heater_board_measure_request(false);
            if (!heater_board_pwm_apply(s_control.applied)) {
                pwm_failure();
            } else {
                heater_board_heat_request(heat);
            }
            notification = EVENT_FRAME_START;
            break;
        }
        case PHASE_DISCHARGE:
            // IN=0 grounds both force terminals without applying 48 V.
            if (!heater_board_pwm_low()) {
                pwm_failure();
            } else if (!s_control.pwm_failed) {
                heater_board_heat_request(true);
            }
            s_control.bootstrap_ready = false;
            break;
        case PHASE_ISOLATE:
            heater_board_heat_request(false);
            break;
        case PHASE_MEASURE_ON:
            if (!s_control.pwm_failed) {
                heater_board_measure_request(true);
            }
            break;
        case PHASE_CONVERT:
            notification = EVENT_CONVERT;
            break;
        case PHASE_MEASURE_OFF:
            // This cutoff is independent of SPI, DRDY, the task and USB.
            heater_board_measure_request(false);
            s_control.started_mask = 0;
            break;
        case PHASE_DONE:
            notification = EVENT_FRAME_DONE;
            break;
        case PHASE_PRECHARGE:
            // PhotoMOS has been off for >800 us; held-low PWM keeps power zero.
            if (!s_control.pwm_failed && heater_board_pwm_low()) {
                heater_board_heat_request(true);
                s_control.bootstrap_ready = true;
            } else {
                pwm_failure();
            }
            break;
        default:
            break;
        }
        s_control.next_phase = phase + 1;
        if (s_control.next_phase == PHASE_COUNT) {
            s_control.next_phase = PHASE_START;
            s_control.next_cycle_start_us += HEATER_PERIOD_US;
        }
        s_control.alarm.alarm_count = s_control.next_cycle_start_us + s_phase_us[s_control.next_phase];
    }
    portEXIT_CRITICAL_ISR(&s_control.lock);
    uint64_t after;
    if (gptimer_get_raw_count(timer, &after) != ESP_OK) {
        portENTER_CRITICAL_ISR(&s_control.lock);
        heater_board_requests_off();
        s_control.pwm_failed = true;
        s_control.faults |= HEATER_FAULT_TIMING;
        s_control.phase = PHASE_ABORT;
        portEXIT_CRITICAL_ISR(&s_control.lock);
        gptimer_stop(timer);
        xTaskNotifyFromISR(s_control.task, EVENT_ABORT, eSetBits, &woken);
        return woken == pdTRUE;
    }
    if (after + ALARM_REARM_GUARD_US >= s_control.alarm.alarm_count) {
        // Driver work inside this callback can also overrun the next phase.
        portENTER_CRITICAL_ISR(&s_control.lock);
        heater_board_requests_off();
        s_control.bootstrap_ready = false;
        s_control.started_mask = 0;
        s_control.healthy_mask = 0;
        s_control.faults |= HEATER_FAULT_TIMING;
        ++s_control.deadline_misses;
        s_control.phase = PHASE_ABORT;
        s_control.next_phase = PHASE_START;
        s_control.next_cycle_start_us = (after / HEATER_PERIOD_US + 1) * HEATER_PERIOD_US;
        if (after + ALARM_REARM_GUARD_US >= s_control.next_cycle_start_us) {
            s_control.next_cycle_start_us += HEATER_PERIOD_US;
        }
        s_control.alarm.alarm_count = s_control.next_cycle_start_us;
        portEXIT_CRITICAL_ISR(&s_control.lock);
        notification = EVENT_ABORT;
    }
    if (gptimer_set_alarm_action(timer, &s_control.alarm) != ESP_OK) {
        portENTER_CRITICAL_ISR(&s_control.lock);
        heater_board_requests_off();
        s_control.pwm_failed = true;
        s_control.faults |= HEATER_FAULT_TIMING;
        s_control.phase = PHASE_ABORT;
        portEXIT_CRITICAL_ISR(&s_control.lock);
        notification |= EVENT_ABORT;
    }
    if (notification) {
        xTaskNotifyFromISR(s_control.task, notification, eSetBits, &woken);
    }
    return woken == pdTRUE;
}

static void IRAM_ATTR adc_ready(void *arg)
{
    const size_t channel = *(uint8_t *)arg;
    const uint8_t bit = 1U << channel;
    uint64_t now;
    if (gptimer_get_raw_count(s_control.timer, &now) != ESP_OK) {
        return;
    }
    bool notify = false;
    portENTER_CRITICAL_ISR(&s_control.lock);
    if (s_control.phase == PHASE_CONVERT && (s_control.started_mask & bit) &&
        now >= s_control.adc_start_us[channel] + ADC_EARLIEST_READY_US &&
        now < s_control.cycle_start_us + MEASURE_OFF_US) {
        // Latch each edge: shared SPI clocks can clear other devices' DRDY.
        s_control.ready_mask |= bit;
        notify = true;
    }
    portEXIT_CRITICAL_ISR(&s_control.lock);
    if (notify) {
        BaseType_t woken = pdFALSE;
        xTaskNotifyFromISR(s_control.task, EVENT_ADC_READY, eSetBits, &woken);
        if (woken) {
            portYIELD_FROM_ISR();
        }
    }
}

static void start_conversion(uint32_t sequence)
{
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
        uint64_t now;
        if (gptimer_get_raw_count(s_control.timer, &now) != ESP_OK) {
            add_fault(sequence, HEATER_FAULT_TIMING);
            return;
        }
        portENTER_CRITICAL(&s_control.lock);
        const bool on_time = s_control.sequence == sequence && s_control.phase == PHASE_CONVERT &&
            !s_control.pwm_failed && now <= s_control.cycle_start_us + ADC_START_DEADLINE_US;
        if (on_time) {
            s_control.adc_start_us[i] = now;
            s_control.started_mask |= 1U << i;
        }
        portEXIT_CRITICAL(&s_control.lock);
        if (!on_time) {
            add_fault(sequence, HEATER_FAULT_TIMING);
            return;
        }
        if (ads1220_start(i) != ESP_OK) {
            portENTER_CRITICAL(&s_control.lock);
            s_control.started_mask &= ~(1U << i);
            portEXIT_CRITICAL(&s_control.lock);
            add_fault(sequence, HEATER_FAULT_SPI);
            continue;
        }
        if (gptimer_get_raw_count(s_control.timer, &now) != ESP_OK) {
            add_fault(sequence, HEATER_FAULT_TIMING);
            return;
        }
        portENTER_CRITICAL(&s_control.lock);
        const bool completed_on_time = s_control.sequence == sequence &&
            s_control.phase == PHASE_CONVERT &&
            now <= s_control.cycle_start_us + ADC_START_DEADLINE_US;
        if (!completed_on_time && s_control.sequence == sequence) {
            s_control.started_mask &= ~(1U << i);
        }
        portEXIT_CRITICAL(&s_control.lock);
        if (!completed_on_time) {
            add_fault(sequence, HEATER_FAULT_TIMING);
            return;
        }
    }
}

static void read_ready(heater_telemetry_t *frame, uint8_t *read_mask)
{
    const snapshot_t current = snapshot();
    if (current.sequence != frame->sequence || current.phase != PHASE_CONVERT) {
        return;
    }
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
        const uint8_t bit = 1U << i;
        if (!(current.ready_mask & bit) || (*read_mask & bit)) {
            continue;
        }
        uint64_t now;
        if (gptimer_get_raw_count(s_control.timer, &now) != ESP_OK ||
            now + ADC_READ_GUARD_US >= frame->cycle_start_us + MEASURE_OFF_US) {
            add_fault(frame->sequence, HEATER_FAULT_TIMING);
            return;
        }
        int32_t code;
        *read_mask |= bit;
        if (ads1220_read(i, &code) != ESP_OK) {
            add_fault(frame->sequence, HEATER_FAULT_SPI);
            continue;
        }
        if (gptimer_get_raw_count(s_control.timer, &now) != ESP_OK ||
            now >= frame->cycle_start_us + MEASURE_OFF_US) {
            add_fault(frame->sequence, HEATER_FAULT_TIMING);
            continue;
        }
        const float resistance = heater_resistance_ohm(code, s_reference_mohm[i], s_offset_mohm[i]);
        if (code <= 0 || code >= 0x7fffff ||
            resistance < CONFIG_HEATER_VALID_MIN_MOHM / 1000.0f ||
            resistance > CONFIG_HEATER_VALID_MAX_MOHM / 1000.0f) {
            add_fault(frame->sequence, HEATER_FAULT_SAMPLE);
            continue;
        }
        frame->resistance_ohm[i] = resistance;
        frame->valid_mask |= bit;
    }
}

static void publish(heater_telemetry_t *frame, bool aborted, uint8_t read_mask)
{
    portENTER_CRITICAL(&s_control.lock);
    if (s_control.sequence != frame->sequence) {
        portEXIT_CRITICAL(&s_control.lock);
        return;
    }
    frame->faults |= s_control.faults;
    if (read_mask != ALL_CHANNELS_MASK) {
        frame->faults |= HEATER_FAULT_ADC_TIMEOUT;
    }
    if (aborted || (frame->faults & (HEATER_FAULT_TIMING | HEATER_FAULT_PWM))) {
        frame->valid_mask = 0;
    }
    s_control.healthy_mask = frame->valid_mask;
    s_control.healthy_sequence = frame->sequence;
    frame->deadline_misses = s_control.deadline_misses;
    frame->telemetry_drops = s_control.telemetry_drops;
    portEXIT_CRITICAL(&s_control.lock);
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
        if (!(frame->valid_mask & (1U << i))) {
            frame->resistance_ohm[i] = NAN;
        }
    }
    frame->control_stack_free_bytes = uxTaskGetStackHighWaterMark(NULL);
    const heater_message_t message = {.kind = HEATER_MESSAGE_DATA, .data = *frame};
    if (xQueueSend(s_control.outbound, &message, 0) != pdPASS) {
        portENTER_CRITICAL(&s_control.lock);
        ++s_control.telemetry_drops;
        portEXIT_CRITICAL(&s_control.lock);
    }
}

static void control_task(void *arg)
{
    (void)arg;
    heater_telemetry_t frame = {0};
    uint8_t read_mask = 0;
    uint32_t published_sequence = 0;
    for (;;) {
        uint32_t events;
        xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
        const snapshot_t current = snapshot();
        if (frame.sequence != current.sequence) {
            frame = (heater_telemetry_t) {
                .sequence = current.sequence,
                .cycle_start_us = current.cycle_start_us,
                .resistance_ohm = {NAN, NAN, NAN, NAN},
            };
            memcpy(frame.duty, current.duty, sizeof(frame.duty));
            read_mask = 0;
        }
        if ((events & EVENT_CONVERT) && current.phase == PHASE_CONVERT) {
            start_conversion(frame.sequence);
        }
        if (events & EVENT_ADC_READY) {
            read_ready(&frame, &read_mask);
        }
        const snapshot_t ending = snapshot();
        const bool aborted = (events & EVENT_ABORT) && ending.phase == PHASE_ABORT;
        const bool done = (events & EVENT_FRAME_DONE) &&
            (ending.phase == PHASE_DONE || ending.phase == PHASE_PRECHARGE);
        if ((done || aborted) && published_sequence != frame.sequence) {
            publish(&frame, aborted, read_mask);
            published_sequence = frame.sequence;
        }
    }
}

esp_err_t heater_control_init(QueueHandle_t outbound)
{
    if (outbound == NULL || s_control.outbound != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_control.outbound = outbound;
    ESP_RETURN_ON_ERROR(heater_board_init(), TAG, "Board initialization failed");
    const esp_err_t adc_result = ads1220_init();
    if (adc_result != ESP_OK) {
        heater_board_requests_off();
        return adc_result;
    }
    const gptimer_config_t timer = {
        .clk_src = GPTIMER_CLK_SRC_DEFAULT,
        .direction = GPTIMER_COUNT_UP,
        .resolution_hz = 1000000,
        .intr_priority = 3,
    };
    ESP_RETURN_ON_ERROR(gptimer_new_timer(&timer, &s_control.timer), TAG, "Phase timer failed");
    const gptimer_event_callbacks_t callbacks = {.on_alarm = phase_alarm};
    ESP_RETURN_ON_ERROR(gptimer_register_event_callbacks(s_control.timer, &callbacks, NULL),
                        TAG, "Phase callback failed");
    const gpio_config_t drdy = {
        .pin_bit_mask = (1ULL << 17) | (1ULL << 18) | (1ULL << 21) | (1ULL << 38),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&drdy), TAG, "DRDY GPIO failed");
    ESP_RETURN_ON_ERROR(gpio_install_isr_service(ESP_INTR_FLAG_IRAM | ESP_INTR_FLAG_LEVEL3),
                        TAG, "DRDY ISR service failed");
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
        ESP_RETURN_ON_ERROR(gpio_isr_handler_add(heater_adc_drdy_gpio[i], adc_ready, &s_drdy_channel[i]),
                            TAG, "DRDY callback failed");
    }
    return ESP_OK;
}

esp_err_t heater_control_start(void)
{
    if (s_control.timer == NULL || s_control.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xTaskCreate(control_task, "heater_cycle", CONTROL_TASK_STACK_BYTES, NULL,
                    CONTROL_TASK_PRIORITY, &s_control.task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_control.next_phase = PHASE_START;
    s_control.next_cycle_start_us = HEATER_PERIOD_US;
    s_control.alarm.alarm_count = HEATER_PERIOD_US;
    ESP_RETURN_ON_ERROR(gptimer_set_alarm_action(s_control.timer, &s_control.alarm),
                        TAG, "Initial phase alarm failed");
    ESP_RETURN_ON_ERROR(gptimer_enable(s_control.timer), TAG, "Phase timer enable failed");
    ESP_LOGI(TAG, "20ms frame, 15ms heat; 10kHz PWM; ADS1220 Turbo 660SPS, gain 4, IDAC 1.5mA");
    return gptimer_start(s_control.timer);
}

esp_err_t heater_control_submit(const heater_command_t *command)
{
    if (command == NULL || s_control.task == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (command->kind == HEATER_COMMAND_SET) {
        for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
            if (command->duty[i] > HEATER_MAX_DUTY) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    } else if (command->kind != HEATER_COMMAND_STOP && command->kind != HEATER_COMMAND_PING) {
        return ESP_ERR_INVALID_ARG;
    }
    if (command->kind == HEATER_COMMAND_PING) {
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_control.lock);
    if (command->kind == HEATER_COMMAND_STOP) {
        s_control.command_active = false;
        s_control.bootstrap_ready = false;
        memset(s_control.desired, 0, sizeof(s_control.desired));
        heater_board_heat_request(false);
        s_control.faults |= HEATER_FAULT_STOPPED;
    } else if (command->kind == HEATER_COMMAND_SET) {
        memcpy(s_control.desired, command->duty, sizeof(s_control.desired));
        s_control.command_active = true;
    }
    portEXIT_CRITICAL(&s_control.lock);
    return ESP_OK;
}
