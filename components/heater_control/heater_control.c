#include "heater_control.h"

#include <math.h>
#include <string.h>

#include "ads1220.h"
#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "heater_board.h"
#include "sdkconfig.h"

#if !CONFIG_GPTIMER_ISR_CACHE_SAFE || !CONFIG_GPTIMER_CTRL_FUNC_IN_IRAM || \
    !CONFIG_MCPWM_CTRL_FUNC_IN_IRAM || !CONFIG_GPIO_CTRL_FUNC_IN_IRAM
#error "Heater phase callbacks require cache-safe GPTimer, MCPWM and GPIO control functions"
#endif

#define CONTROL_TASK_STACK_BYTES 6144
#define CONTROL_TASK_PRIORITY 20
#define ADC_START_US 16800U
// Wi-Fi/PSRAM 缓存波动使四路 START 即使在 240 MHz 下也可能超过 200 µs。
// 允许到 17.10 ms 完成，但测量关断仍固定为周期起点后的 19.05 ms。
#define ADC_START_DEADLINE_US (ADC_START_US + 300U)
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
#define EVENT_PAUSE (1U << 5)

// 定义一个 20 ms 周期的加热、互锁、转换、关断和预充电阶段；ABORT 表示异常帧。
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

// 阶段表放入可写 DRAM，确保 IRAM ISR 在 Flash 缓存不可用时仍可读取。
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

// 保存控制任务与 ISR 共用的周期、许可、运行和维护状态，驻留内部 RAM。
// 运行期共享状态用 lock 保护；暂停/恢复用 lifecycle 串行化，pause_ack 确认停止。
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
    uint32_t run_id;
    uint32_t frame_run_id;
    bool latest_present;
    heater_telemetry_t latest;
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
    bool maintenance;
    bool paused;
    bool board_ready;
    bool host_timeout;
    uint64_t lease_deadline_us;
    int64_t pause_start_us;
    SemaphoreHandle_t pause_ack;
    SemaphoreHandle_t lifecycle;
    heater_timing_stats_t timing;
} control_t;

static control_t s_control = {.lock = portMUX_INITIALIZER_UNLOCKED};

// 保存一次短临界区读取的控制快照，供任务在锁外处理，可能随下一中断过期。
typedef struct {
    phase_t phase;
    uint32_t sequence;
    uint64_t cycle_start_us;
    uint8_t duty[HEATER_CHANNEL_COUNT];
    uint8_t ready_mask;
    uint32_t faults;
} snapshot_t;

// 在短临界区内复制阶段、周期及通道状态，供任务在锁外处理采样。
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

// 仅当 `sequence` 仍对应当前周期时合并 `fault`，避免迟到操作污染下一帧。
static void add_fault(uint32_t sequence, uint32_t fault)
{
    portENTER_CRITICAL(&s_control.lock);
    if (s_control.sequence == sequence) {
        s_control.faults |= fault;
    }
    portEXIT_CRITICAL(&s_control.lock);
}

// 关闭请求并锁存 PWM 故障，禁止后续加热直到重启。
// 调用方须已持有控制状态锁；函数位于 IRAM，供阶段中断调用。
static void IRAM_ATTR pwm_failure(void)
{
    heater_board_requests_off();
    s_control.pwm_failed = true;
    s_control.bootstrap_ready = false;
    s_control.faults |= HEATER_FAULT_PWM;
}

// 在 GPTimer ISR 中推进绝对阶段时序，切换请求/PWM 并通知控制任务。
// `timer`、`event` 由驱动提供且非空，`user_data` 未使用；返回是否需切换任务。
// 迟到或重设闹钟失败时立即关断并丢弃该帧，恢复闹钟只安排到未来边界。
// 仅执行有界的 IRAM 操作与 FromISR 通知，不在中断中进行 SPI、日志或换算。
static bool IRAM_ATTR phase_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t *event,
                                 void *user_data)
{
    (void)user_data;
    uint32_t notification = 0;
    BaseType_t woken = pdFALSE;
    portENTER_CRITICAL_ISR(&s_control.lock);
    if (s_control.maintenance) {
        heater_board_requests_off();
        portEXIT_CRITICAL_ISR(&s_control.lock);
        return false;
    }
    const phase_t phase = s_control.next_phase;
    s_control.phase = phase;
    if (event->count_value > event->alarm_value + CONFIG_HEATER_MAX_ISR_LATENCY_US) {
        // 关断后只在未来的绝对周期边界恢复，避免将过去时间重新设为闹钟。
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
            if (s_control.command_active && event->count_value >= s_control.lease_deadline_us) {
                s_control.command_active = false;
                s_control.host_timeout = true;
                memset(s_control.desired, 0, sizeof(s_control.desired));
            }
            s_control.frame_run_id = s_control.command_active ? s_control.run_id : 0;
            s_control.faults = (s_control.pwm_failed ? HEATER_FAULT_PWM : 0) |
                (s_control.host_timeout ? HEATER_FAULT_HOST_TIMEOUT : 0);
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
            // IN=0 将两个电流端接地放电，不施加 48 V 加热功率。
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
            // 由定时器中断强制截止，不依赖 SPI、DRDY、控制任务或 USB 的完成情况。
            heater_board_measure_request(false);
            s_control.started_mask = 0;
            break;
        case PHASE_DONE:
            notification = EVENT_FRAME_DONE;
            break;
        case PHASE_PRECHARGE:
            // PhotoMOS 已关闭超过 800 µs；PWM 保持为低，仅给自举电容预充电。
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
        // 回调内部的驱动操作也可能耗尽下一阶段裕量，重新检查并只安排未来闹钟。
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

// 锁存转换窗口内的 DRDY 下降沿，仅接收已启动且满足最早就绪时间的通道。
// `arg` 指向常驻内部 RAM 的通道索引；ISR 仅标记就绪并通知任务读取。
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
    if (!s_control.maintenance && s_control.phase == PHASE_CONVERT && (s_control.started_mask & bit) &&
        now >= s_control.adc_start_us[channel] + ADC_EARLIEST_READY_US &&
        now < s_control.cycle_start_us + MEASURE_OFF_US) {
        // 分别锁存各通道下降沿，避免共享 SPI 时钟清除其他 ADC 的 DRDY 后丢失就绪状态。
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

// 在任务中依次启动四路 ADC，每条命令前后检查阶段、周期及 17.10 ms 截止。
// 只记录 `sequence` 的故障；提前登记启动时间供 DRDY ISR 检查，失败撤销许可。
static void start_conversion(uint32_t sequence)
{
    for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
        uint64_t now;
        if (gptimer_get_raw_count(s_control.timer, &now) != ESP_OK) {
            add_fault(sequence, HEATER_FAULT_TIMING);
            return;
        }
        portENTER_CRITICAL(&s_control.lock);
        const bool on_time = !s_control.maintenance && s_control.sequence == sequence && s_control.phase == PHASE_CONVERT &&
            !s_control.pwm_failed && now <= s_control.cycle_start_us + ADC_START_DEADLINE_US;
        if (on_time) {
            s_control.adc_start_us[i] = now;
            s_control.started_mask |= 1U << i;
        }
        portEXIT_CRITICAL(&s_control.lock);
        if (!on_time) {
            portENTER_CRITICAL(&s_control.lock);
            ++s_control.timing.adc_start_misses;
            const uint32_t offset = now > s_control.cycle_start_us ? (uint32_t)(now - s_control.cycle_start_us) : 0;
            if (offset > s_control.timing.adc_start_max_offset_us) s_control.timing.adc_start_max_offset_us = offset;
            portEXIT_CRITICAL(&s_control.lock);
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
        const bool completed_on_time = !s_control.maintenance && s_control.sequence == sequence &&
            s_control.phase == PHASE_CONVERT &&
            now <= s_control.cycle_start_us + ADC_START_DEADLINE_US;
        if (s_control.sequence == sequence) {
            const uint32_t offset = (uint32_t)(now - s_control.cycle_start_us);
            if (offset > s_control.timing.adc_start_max_offset_us) s_control.timing.adc_start_max_offset_us = offset;
            if (!completed_on_time) ++s_control.timing.adc_start_misses;
        }
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

// 读取 `frame` 当前周期已锁存且未读的通道，更新电阻和有效掩码。
// 非空的 `read_mask` 按通道记录已尝试读取，避免 SPI 失败后重复读取。
// SPI 前预留截止裕量，返回后再次检查截止；饱和或越界电阻不获得通道许可。
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
            portENTER_CRITICAL(&s_control.lock); ++s_control.timing.adc_read_misses; portEXIT_CRITICAL(&s_control.lock);
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
            portENTER_CRITICAL(&s_control.lock); ++s_control.timing.adc_read_misses; portEXIT_CRITICAL(&s_control.lock);
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

// 汇总 `frame` 的故障，按 `aborted` 和 `read_mask` 判定有效性并更新下周期许可。
// 无效电阻写为 NAN；内部安全快照始终更新，只有当前显式运行才非阻塞入队。
// `frame` 须非空；跨周期结果丢弃，队列满仅累计丢帧，不等待缓存或网络。
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
    portENTER_CRITICAL(&s_control.lock);
    if (s_control.sequence != frame->sequence) {
        portEXIT_CRITICAL(&s_control.lock);
        return;
    }
    s_control.latest = *frame;
    s_control.latest_present = true;
    const bool record = s_control.command_active && s_control.frame_run_id != 0 &&
                        s_control.frame_run_id == s_control.run_id;
    const uint32_t run_id = s_control.frame_run_id;
    portEXIT_CRITICAL(&s_control.lock);
    if (!record) return;
    const heater_message_t message = {.kind = HEATER_MESSAGE_DATA, .run_id = run_id, .data = *frame};
    if (xQueueSend(s_control.outbound, &message, 0) != pdPASS) {
        portENTER_CRITICAL(&s_control.lock);
        ++s_control.telemetry_drops;
        portEXIT_CRITICAL(&s_control.lock);
    }
}

// 阻塞等待阶段、DRDY 和维护通知，在任务上下文执行 SPI、换算及发布。
// 维护时停止 GPTimer、确认 PWM 为低后应答；每个周期最多发布一次。
// `arg` 未使用；heater_cycle 位于 Core 1，优先级 20，栈为 6144 字节。
static void control_task(void *arg)
{
    (void)arg;
    heater_telemetry_t frame = {0};
    uint8_t read_mask = 0;
    uint32_t published_sequence = 0;
    for (;;) {
        uint32_t events;
        xTaskNotifyWait(0, UINT32_MAX, &events, portMAX_DELAY);
        portENTER_CRITICAL(&s_control.lock);
        const bool maintenance = s_control.maintenance;
        portEXIT_CRITICAL(&s_control.lock);
        if (maintenance) {
            if (events & EVENT_PAUSE) {
                const esp_err_t stopped = gptimer_stop(s_control.timer);
                portENTER_CRITICAL(&s_control.lock);
                heater_board_requests_off();
                const bool low = heater_board_pwm_low();
                s_control.paused = stopped == ESP_OK && low;
                s_control.pause_start_us = esp_timer_get_time();
                portEXIT_CRITICAL(&s_control.lock);
                xSemaphoreGive(s_control.pause_ack);
            }
            continue;
        }
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

// 创建暂停应答和生命周期锁，再依次初始化板级输出、ADC、GPTimer 与 DRDY ISR。
// 中断归属调用所在核心；ADC 配置失败时关闭请求并保留板级恢复能力。
esp_err_t heater_control_init(QueueHandle_t outbound)
{
    if (outbound == NULL || s_control.outbound != NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_control.outbound = outbound;
    s_control.pause_ack = xSemaphoreCreateBinary();
    s_control.lifecycle = xSemaphoreCreateMutex();
    if (s_control.pause_ack == NULL || s_control.lifecycle == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ESP_RETURN_ON_ERROR(heater_board_init(), TAG, "Board initialization failed");
    s_control.board_ready = true;
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

// 创建与阶段中断同核的高优先级控制任务，安排首个绝对周期闹钟后启动计时。
esp_err_t heater_control_start(void)
{
    if (s_control.timer == NULL || s_control.task != NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    // 启动初始化及中断分配同样位于 Core 1，见 app_main。
    if (xTaskCreatePinnedToCore(control_task, "heater_cycle", CONTROL_TASK_STACK_BYTES, NULL,
                    CONTROL_TASK_PRIORITY, &s_control.task, 1) != pdPASS) {
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

// 在临界区内更新设定、运行 ID 和租约；STOP 直接关断，SET/PING 不启动空闲运行。
// 尚无控制任务时仅允许对已初始化板级输出执行 STOP。
esp_err_t heater_control_submit(const heater_command_t *command)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_control.task == NULL) {
        if (command->kind != HEATER_COMMAND_STOP || !s_control.board_ready) return ESP_ERR_INVALID_STATE;
        heater_board_requests_off();
        return heater_board_pwm_low() ? ESP_OK : ESP_FAIL;
    }
    if (command->kind == HEATER_COMMAND_SET || command->kind == HEATER_COMMAND_START) {
        for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
            if (command->duty[i] > HEATER_MAX_DUTY || command->duty[i] > CONFIG_HEATER_OUTPUT_DUTY_LIMIT) {
                return ESP_ERR_INVALID_ARG;
            }
        }
    } else if (command->kind != HEATER_COMMAND_STOP && command->kind != HEATER_COMMAND_PING) {
        return ESP_ERR_INVALID_ARG;
    }
    uint64_t now;
    if (gptimer_get_raw_count(s_control.timer, &now) != ESP_OK) return ESP_FAIL;
    portENTER_CRITICAL(&s_control.lock);
    if (s_control.maintenance && command->kind != HEATER_COMMAND_STOP) {
        portEXIT_CRITICAL(&s_control.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (command->kind == HEATER_COMMAND_STOP) {
        s_control.command_active = false;
        s_control.bootstrap_ready = false;
        memset(s_control.desired, 0, sizeof(s_control.desired));
        heater_board_heat_request(false);
        heater_board_pwm_low();
        s_control.host_timeout = false;
        s_control.faults |= HEATER_FAULT_STOPPED;
    } else if (command->kind == HEATER_COMMAND_SET || command->kind == HEATER_COMMAND_START) {
        memcpy(s_control.desired, command->duty, sizeof(s_control.desired));
        if (command->kind == HEATER_COMMAND_START) {
            if (++s_control.run_id == 0) ++s_control.run_id;
            s_control.command_active = true;
        }
        if (s_control.command_active) {
            s_control.host_timeout = false;
            s_control.lease_deadline_us = now + (uint64_t)CONFIG_HEATER_CONTROL_LEASE_MS * 1000;
        }
    } else if (s_control.command_active) {
        s_control.lease_deadline_us = now + (uint64_t)CONFIG_HEATER_CONTROL_LEASE_MS * 1000;
    }
    portEXIT_CRITICAL(&s_control.lock);
    return ESP_OK;
}

// 在控制锁内同时读取运行标志和 ID，使缓存任务能一致地筛选入队样本。
heater_run_state_t heater_control_run_state(void)
{
    portENTER_CRITICAL(&s_control.lock);
    const heater_run_state_t state = {.running = s_control.command_active && !s_control.maintenance,
                                      .id = s_control.run_id};
    portEXIT_CRITICAL(&s_control.lock);
    return state;
}

// 在控制锁内复制最新内部采样；未发布过帧时保留调用方输出。
bool heater_control_latest(heater_telemetry_t *frame)
{
    portENTER_CRITICAL(&s_control.lock);
    const bool present = s_control.latest_present;
    if (present) *frame = s_control.latest;
    portEXIT_CRITICAL(&s_control.lock);
    return present;
}

// 在控制锁内读取任务已确认的暂停标志，区别于仅发起维护请求。
bool heater_control_is_paused(void)
{
    portENTER_CRITICAL(&s_control.lock);
    const bool paused = s_control.paused;
    portEXIT_CRITICAL(&s_control.lock);
    return paused;
}

// 在控制锁内复制启动最大偏移及两类截止失败计数。
heater_timing_stats_t heater_control_timing_stats(void)
{
    portENTER_CRITICAL(&s_control.lock);
    const heater_timing_stats_t result = s_control.timing;
    portEXIT_CRITICAL(&s_control.lock);
    return result;
}

// 在控制锁内读取维护请求标志，该标志可先于实际暂停确认生效。
bool heater_control_is_maintenance(void)
{
    portENTER_CRITICAL(&s_control.lock);
    const bool paused = s_control.maintenance;
    portEXIT_CRITICAL(&s_control.lock);
    return paused;
}

// 用生命周期互斥锁串行化暂停/恢复，先关断并清空运行，再等待任务应答。
// 无控制任务时直接核验板级关断；超时保持维护状态，禁止写 Flash。
esp_err_t heater_control_pause(TickType_t timeout)
{
    if (s_control.lifecycle == NULL) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_control.lifecycle, timeout) != pdTRUE) return ESP_ERR_TIMEOUT;
    if (s_control.task == NULL) {
        // 初始化结束但未启动控制任务，例如 ADC 寄存器验证失败。
        // 此时没有运行中的 SPI 事务或周期定时器，可直接核验板级关断。
        portENTER_CRITICAL(&s_control.lock);
        const bool ready = s_control.board_ready;
        if (ready) {
            s_control.maintenance = true;
            s_control.command_active = false;
            heater_board_requests_off();
            s_control.paused = heater_board_pwm_low();
        }
        const bool safe = ready && s_control.paused;
        portEXIT_CRITICAL(&s_control.lock);
        xSemaphoreGive(s_control.lifecycle);
        return safe ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_control.lock);
    const bool paused = s_control.paused;
    s_control.maintenance = true;
    s_control.command_active = false;
    s_control.bootstrap_ready = false;
    s_control.started_mask = s_control.ready_mask = s_control.healthy_mask = 0;
    memset(s_control.desired, 0, sizeof(s_control.desired));
    memset(s_control.applied, 0, sizeof(s_control.applied));
    heater_board_requests_off();
    heater_board_pwm_low();
    portEXIT_CRITICAL(&s_control.lock);
    esp_err_t result = ESP_OK;
    if (!paused) {
        xSemaphoreTake(s_control.pause_ack, 0); // 丢弃之前失败流程遗留的应答
        xTaskNotify(s_control.task, EVENT_PAUSE, eSetBits);
        if (xSemaphoreTake(s_control.pause_ack, timeout) != pdTRUE || !s_control.paused) {
            result = ESP_ERR_TIMEOUT;
        }
    }
    xSemaphoreGive(s_control.lifecycle);
    return result;
}

// 串行化恢复操作，将实际暂停时长补入 GPTimer 并安排未来的周期边界。
// 启动失败重新锁定维护状态；成功后仍为空闲，需 START 创建新的显式运行。
esp_err_t heater_control_resume(void)
{
    if (s_control.lifecycle == NULL || s_control.task == NULL || s_control.timer == NULL)
        return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_control.lifecycle, pdMS_TO_TICKS(1000)) != pdTRUE) return ESP_ERR_TIMEOUT;
    esp_err_t result = ESP_ERR_INVALID_STATE;
    if (s_control.paused) {
        uint64_t now;
        result = gptimer_get_raw_count(s_control.timer, &now);
        if (result == ESP_OK) {
            now += (uint64_t)(esp_timer_get_time() - s_control.pause_start_us);
            result = gptimer_set_raw_count(s_control.timer, now);
        }
        if (result == ESP_OK) {
            s_control.next_cycle_start_us = (now / HEATER_PERIOD_US + 2) * HEATER_PERIOD_US;
            s_control.next_phase = PHASE_START;
            s_control.healthy_sequence = s_control.sequence;
            s_control.alarm.alarm_count = s_control.next_cycle_start_us;
            result = gptimer_set_alarm_action(s_control.timer, &s_control.alarm);
        }
        if (result == ESP_OK) {
            portENTER_CRITICAL(&s_control.lock);
            s_control.maintenance = false;
            s_control.paused = false;
            portEXIT_CRITICAL(&s_control.lock);
            result = gptimer_start(s_control.timer);
            if (result != ESP_OK) {
                portENTER_CRITICAL(&s_control.lock);
                s_control.maintenance = true;
                s_control.paused = true;
                heater_board_requests_off();
                portEXIT_CRITICAL(&s_control.lock);
            }
        }
    }
    xSemaphoreGive(s_control.lifecycle);
    return result;
}
