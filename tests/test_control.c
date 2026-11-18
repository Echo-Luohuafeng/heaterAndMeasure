// Exercise the actual ISR/state logic with fake clocks, ADCs, GPIOs and queues.
// These tests cover event ordering, missed deadlines and fail-closed behavior;
// they do not simulate analog settling, hardware IRQ jitter or ADC accuracy.
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>

#include "../components/heater_control/heater_control.c"

static uint64_t fake_now;
static bool heat_request, measure_request;
static uint8_t pwm[4];
static bool queue_full, pwm_ok;
static uint32_t driver_time_us;
static uint32_t adc_start_time_us;
static heater_message_t last_message;
static uint32_t sent_messages, notifications;
static int32_t adc_codes[4];
static bool drive_task;
static unsigned task_waits;
static unsigned task_frame_limit;
static bool check_hold;
static bool drive_pause, pause_ack, pause_timeout, timer_running;
static uint64_t stopped_counter;
static jmp_buf task_escape;

const int heater_pwm_gpio[4] = {4, 5, 6, 7};
const int heater_adc_cs_gpio[4] = {8, 9, 10, 14};
const int heater_adc_drdy_gpio[4] = {17, 18, 21, 38};
esp_err_t heater_board_init(void) { return ESP_OK; }
void heater_board_requests_off(void) { heat_request = measure_request = false; }
void heater_board_heat_request(bool enabled) { assert(!enabled || !measure_request); heat_request = enabled; }
void heater_board_measure_request(bool enabled) { assert(!enabled || !heat_request); measure_request = enabled; }
bool heater_board_pwm_low(void) { memset(pwm, 0, sizeof(pwm)); fake_now += driver_time_us; return pwm_ok; }
bool heater_board_pwm_apply(const uint8_t duty[4]) { memcpy(pwm, duty, 4); fake_now += driver_time_us; return pwm_ok; }
esp_err_t ads1220_init(void) { return ESP_OK; }
esp_err_t ads1220_start(size_t channel) { (void)channel; fake_now += adc_start_time_us; return ESP_OK; }
esp_err_t ads1220_read(size_t channel, int32_t *code) { *code = adc_codes[channel]; fake_now += 40; return ESP_OK; }
esp_err_t gptimer_get_raw_count(gptimer_handle_t timer, uint64_t *now) { (void)timer; *now = timer_running ? fake_now : stopped_counter; return ESP_OK; }
esp_err_t gptimer_set_raw_count(gptimer_handle_t timer, uint64_t now) { (void)timer; stopped_counter = now; return ESP_OK; }
int64_t esp_timer_get_time(void) { return (int64_t)fake_now; }
esp_err_t gptimer_set_alarm_action(gptimer_handle_t timer, const gptimer_alarm_config_t *alarm)
{ (void)timer; assert(alarm->alarm_count > fake_now); return ESP_OK; }
esp_err_t gptimer_new_timer(const gptimer_config_t *config, gptimer_handle_t *timer)
{ (void)config; *timer = (void *)1; return ESP_OK; }
esp_err_t gptimer_register_event_callbacks(gptimer_handle_t timer, const gptimer_event_callbacks_t *cb, void *arg)
{ (void)timer; (void)cb; (void)arg; return ESP_OK; }
esp_err_t gptimer_enable(gptimer_handle_t timer) { (void)timer; return ESP_OK; }
esp_err_t gptimer_start(gptimer_handle_t timer) { (void)timer; timer_running = true; return ESP_OK; }
esp_err_t gptimer_stop(gptimer_handle_t timer) { (void)timer; stopped_counter = fake_now; timer_running = false; return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *config) { (void)config; return ESP_OK; }
esp_err_t gpio_install_isr_service(int flags) { (void)flags; return ESP_OK; }
esp_err_t gpio_isr_handler_add(int pin, void (*cb)(void *), void *arg)
{ (void)pin; (void)cb; (void)arg; return ESP_OK; }
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, uint32_t stack, void *arg,
                      UBaseType_t priority, TaskHandle_t *task)
{ (void)entry; (void)name; (void)stack; (void)arg; (void)priority; *task = (void *)1; return pdPASS; }
BaseType_t xTaskCreatePinnedToCore(void (*entry)(void *), const char *name, uint32_t stack, void *arg,
                                 UBaseType_t priority, TaskHandle_t *task, BaseType_t core)
{ assert(core == 1); return xTaskCreate(entry, name, stack, arg, priority, task); }
BaseType_t xTaskNotify(TaskHandle_t task, uint32_t bits, int action)
{ (void)task; (void)action; notifications |= bits; return pdPASS; }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return (void *)3; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)2; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{ if (semaphore == (void *)3) pause_ack = true; return pdTRUE; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t timeout)
{
    if (semaphore == (void *)2) return pdTRUE;
    assert(semaphore == (void *)3);
    if (timeout && !pause_timeout && !pause_ack) {
        drive_pause = true; task_waits = 0;
        if (setjmp(task_escape) == 0) control_task(NULL);
        drive_pause = false;
    }
    const bool acknowledged = pause_ack;
    pause_ack = false;
    return acknowledged ? pdTRUE : pdFALSE;
}
BaseType_t xTaskNotifyFromISR(TaskHandle_t task, uint32_t bits, int action, BaseType_t *woken)
{ (void)task; (void)action; notifications |= bits; *woken = pdFALSE; return pdPASS; }
static void fire(uint32_t lateness);
BaseType_t xTaskNotifyWait(uint32_t entry, uint32_t exit_bits, uint32_t *bits, TickType_t wait)
{
    (void)entry; (void)exit_bits; (void)wait;
    if (drive_pause && task_waits++) longjmp(task_escape, 1);
    if (drive_task) {
        if (task_waits == task_frame_limit * 4) longjmp(task_escape, 1);
        switch (task_waits++ % 4) {
        case 0:
            if (check_hold) {
                const heater_command_t heartbeat = {.kind = HEATER_COMMAND_PING};
                assert(heater_control_submit(&heartbeat) == ESP_OK);
            }
            while (s_control.next_phase != PHASE_START) fire(0);
            fire(0);
            break;
        case 1:
            while (s_control.next_phase != PHASE_CONVERT) fire(0);
            fire(0);
            break;
        case 2:
            fake_now = s_control.cycle_start_us + 18650;
            for (size_t i = 0; i < 4; ++i) adc_ready(&s_drdy_channel[i]);
            break;
        case 3:
            while (s_control.next_phase != PHASE_DONE) fire(0);
            fire(0);
            break;
        }
    }
    *bits = notifications; notifications = 0; return pdPASS;
}
BaseType_t xQueueSend(QueueHandle_t queue, const void *message, TickType_t wait)
{
    (void)queue;
    assert(wait == 0);
    if (queue_full) return pdFALSE;
    last_message = *(const heater_message_t *)message;
    if (check_hold && last_message.data.sequence > 1) {
        const uint8_t held_duty[4] = {5, 5, 5, 5};
        assert(memcmp(last_message.data.duty, held_duty, sizeof(held_duty)) == 0);
        assert(last_message.data.valid_mask == 15 && last_message.data.faults == 0);
    }
    ++sent_messages;
    return pdPASS;
}
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task) { (void)task; return 2048; }

static void reset(void)
{
    memset(&s_control, 0, sizeof(s_control));
    s_control.timer = s_control.task = s_control.outbound = (void *)1;
    s_control.lifecycle = (void *)2;
    s_control.pause_ack = (void *)3;
    s_control.next_cycle_start_us = 20000;
    s_control.alarm.alarm_count = 20000;
    s_control.next_phase = PHASE_START;
    fake_now = notifications = driver_time_us = sent_messages = 0;
    heat_request = measure_request = queue_full = false;
    pwm_ok = true;
    adc_start_time_us = 12;
    drive_task = false;
    task_waits = 0;
    task_frame_limit = 2;
    check_hold = false;
    drive_pause = pause_ack = pause_timeout = false;
    timer_running = true;
    memset(pwm, 0, sizeof(pwm));
    for (size_t i = 0; i < 4; ++i) adc_codes[i] = 3355443;
}

static void reset_recording(void)
{
    reset();
    const heater_command_t start = {.kind = HEATER_COMMAND_START};
    assert(heater_control_submit(&start) == ESP_OK);
}

static void fire(uint32_t lateness)
{
    const gptimer_alarm_event_data_t event = {
        .alarm_value = s_control.alarm.alarm_count,
        .count_value = s_control.alarm.alarm_count + lateness,
    };
    fake_now = event.count_value;
    phase_alarm(s_control.timer, &event, NULL);
    assert(!(heat_request && measure_request));
}

static heater_telemetry_t frame(void)
{
    const snapshot_t current = snapshot();
    heater_telemetry_t data = {
        .sequence = current.sequence, .cycle_start_us = current.cycle_start_us,
        .resistance_ohm = {NAN, NAN, NAN, NAN},
    };
    memcpy(data.duty, current.duty, 4);
    return data;
}

static void reach_conversion(void)
{
    while (s_control.next_phase != PHASE_CONVERT) fire(0);
    fire(0);
    start_conversion(s_control.sequence);
}

int main(void)
{
    if (CONFIG_HEATER_OUTPUT_DUTY_LIMIT == 2) {
        reset();
        const heater_command_t allowed = {.kind = HEATER_COMMAND_START, .duty = {2, 2, 2, 2}};
        assert(heater_control_submit(&allowed) == ESP_OK);
        for (size_t channel = 0; channel < 4; ++channel) {
            heater_command_t above = allowed;
            above.duty[channel] = 3;
            assert(heater_control_submit(&above) == ESP_ERR_INVALID_ARG);
            above.kind = HEATER_COMMAND_SET;
            assert(heater_control_submit(&above) == ESP_ERR_INVALID_ARG);
            assert(memcmp(s_control.desired, allowed.duty, 4) == 0);
        }
        const heater_command_t stopped = {.kind = HEATER_COMMAND_STOP};
        assert(heater_control_submit(&stopped) == ESP_OK);
        assert(!s_control.command_active && !heat_request);
        puts("Duty limit 2 rejects START/SET above limit on every channel");
        return 0;
    }

    // Idle safety sampling must not enqueue any historical samples. SET stages
    // duties without enabling heat/recording, and PING cannot start an idle run.
    reset();
    const heater_command_t staged = {.kind = HEATER_COMMAND_SET, .duty = {2, 1, 0, 2}};
    assert(heater_control_submit(&staged) == ESP_OK);
    assert(!heater_control_run_state().running);
    drive_task = true; task_frame_limit = 4;
    if (setjmp(task_escape) == 0) control_task(NULL);
    drive_task = false;
    heater_telemetry_t private_sample;
    assert(sent_messages == 0 && !heat_request);
    assert(heater_control_latest(&private_sample) && private_sample.valid_mask == 15);
    // An explicit zero-duty START records; STOP closes that run and idle
    // samples keep the safety state fresh without growing the output queue.
    const heater_command_t start_zero = {.kind = HEATER_COMMAND_START};
    const heater_command_t stop_zero = {.kind = HEATER_COMMAND_STOP};
    assert(heater_control_submit(&start_zero) == ESP_OK);
    const uint32_t first_run = heater_control_run_state().id;
    task_waits = 0; task_frame_limit = 3; drive_task = true;
    if (setjmp(task_escape) == 0) control_task(NULL);
    drive_task = false;
    assert(sent_messages == 3 && last_message.run_id == first_run);
    assert(heater_control_submit(&stop_zero) == ESP_OK);
    task_waits = 0; task_frame_limit = 3; drive_task = true;
    if (setjmp(task_escape) == 0) control_task(NULL);
    drive_task = false;
    assert(sent_messages == 3 && !heater_control_run_state().running);
    assert(heater_control_submit(&start_zero) == ESP_OK);
    assert(heater_control_run_state().id != first_run);

    // Real-board timings: 24 us to wake the task, 34 us per software-CS START.
    // The old 100 us window stopped before ADC4 and invalidated all readings.
    reset_recording();
    adc_start_time_us = 34;
    fire(0);
    while (s_control.next_phase != PHASE_CONVERT) fire(0);
    fire(0);
    fake_now += 24;
    start_conversion(s_control.sequence);
    assert(s_control.started_mask == 15 && s_control.faults == 0);
    assert(fake_now == s_control.cycle_start_us + ADC_START_US + 160);
    fake_now = s_control.cycle_start_us + 18750;
    for (size_t i = 0; i < 4; ++i) adc_ready(&s_drdy_channel[i]);
    heater_telemetry_t actual_timing = frame();
    uint8_t actual_read_mask = 0;
    read_ready(&actual_timing, &actual_read_mask);
    fire(0);
    fire(0);
    publish(&actual_timing, false, actual_read_mask);
    assert(last_message.data.valid_mask == 15 && last_message.data.faults == 0);

    // A command that starts in time but finishes too late must still be rejected.
    reset_recording();
    fire(0);
    while (s_control.next_phase != PHASE_CONVERT) fire(0);
    fire(0);
    fake_now = s_control.cycle_start_us + ADC_START_DEADLINE_US - 5;
    start_conversion(s_control.sequence);
    assert(s_control.started_mask == 0 && (s_control.faults & HEATER_FAULT_TIMING));

    reset_recording();
    heater_command_t set = {.kind = HEATER_COMMAND_START, .duty = {1, 25, 50, 95}};
    assert(heater_control_submit(&set) == ESP_OK);
    fire(0);
    assert(!heat_request); // A command alone does not bypass initial measurement.
    heater_telemetry_t first = frame();
    reach_conversion();
    // Old DRDY edges left over from initialization must not qualify.
    adc_ready(&s_drdy_channel[0]);
    assert(s_control.ready_mask == 0);
    fake_now = first.cycle_start_us + 18650;
    for (size_t i = 0; i < 4; ++i) adc_ready(&s_drdy_channel[i]);
    assert(s_control.ready_mask == 15);
    uint8_t read_mask = 0;
    read_ready(&first, &read_mask);
    assert(first.valid_mask == 15 && read_mask == 15);
    fire(0); // Measurement cutoff still occurs without any task response.
    assert(!measure_request && s_control.started_mask == 0);
    fire(0); // Done
    publish(&first, false, read_mask);
    assert(last_message.data.valid_mask == 15);
    fire(0); // Precharge
    assert(heat_request && pwm[0] == 0 && pwm[3] == 0);
    assert(snapshot().cycle_start_us == first.cycle_start_us); // Frame identity stays stable.
    fire(0); // Next frame
    assert(heat_request && memcmp(pwm, set.duty, 4) == 0);
    assert(s_control.cycle_start_us == 40000);
    const heater_command_t stop = {.kind = HEATER_COMMAND_STOP};
    assert(heater_control_submit(&stop) == ESP_OK);
    assert(!heat_request && (s_control.faults & HEATER_FAULT_STOPPED));

    // Missing one converter inhibits only that channel after the next frame.
    reset_recording();
    assert(heater_control_submit(&set) == ESP_OK);
    fire(0);
    heater_telemetry_t partial = frame();
    reach_conversion();
    fake_now = partial.cycle_start_us + 18650;
    for (size_t i = 0; i < 3; ++i) adc_ready(&s_drdy_channel[i]);
    read_mask = 0;
    read_ready(&partial, &read_mask);
    fire(0);
    fire(0);
    publish(&partial, false, read_mask);
    assert(partial.valid_mask == 7 && (partial.faults & HEATER_FAULT_ADC_TIMEOUT));
    fire(0);
    fire(0);
    assert(pwm[0] == 1 && pwm[1] == 25 && pwm[2] == 50 && pwm[3] == 0);

    // Malformed/saturated data is a sample fault, not a DRDY timeout.
    reset_recording();
    fire(0);
    heater_telemetry_t bad_sample = frame();
    reach_conversion();
    adc_codes[2] = 0x7fffff;
    fake_now = bad_sample.cycle_start_us + 18650;
    for (size_t i = 0; i < 4; ++i) adc_ready(&s_drdy_channel[i]);
    read_mask = 0;
    read_ready(&bad_sample, &read_mask);
    fire(0);
    fire(0);
    publish(&bad_sample, false, read_mask);
    assert(bad_sample.valid_mask == 11 && isnan(bad_sample.resistance_ohm[2]));
    assert(bad_sample.faults & HEATER_FAULT_SAMPLE);
    assert(!(bad_sample.faults & HEATER_FAULT_ADC_TIMEOUT));

    // USB backpressure never prevents hardware measurement cutoff.
    reset_recording();
    fire(0);
    heater_telemetry_t dropped = frame();
    reach_conversion();
    queue_full = true;
    fire(0);
    assert(!measure_request);
    fire(0);
    publish(&dropped, false, 0);
    assert(s_control.telemetry_drops == 1);
    assert(s_control.alarm.alarm_count == 39900);

    // An IRQ arriving several frames late must schedule a future alarm.
    reset_recording();
    fire(45000);
    assert(s_control.phase == PHASE_ABORT && s_control.alarm.alarm_count == 80000);
    assert(!heat_request && !measure_request);
    assert(s_control.deadline_misses == 1);
    // A callback that enters on time can also overrun its next phase.
    reset_recording();
    fire(0);
    driver_time_us = 150;
    fire(0);
    assert(s_control.phase == PHASE_ABORT && !heat_request);
    assert(s_control.alarm.alarm_count > fake_now);
    // Even an unexpectedly long callback must not schedule an expired alarm.
    reset_recording();
    fire(0);
    driver_time_us = 50000;
    fire(0);
    assert(s_control.phase == PHASE_ABORT && !heat_request && !measure_request);
    assert(s_control.alarm.alarm_count == 100000);

    // If the control task never finishes a frame, cached healthy readings
    // cannot keep heating enabled at the next boundary.
    reset_recording();
    s_control.healthy_mask = 15;
    s_control.bootstrap_ready = true;
    assert(heater_control_submit(&set) == ESP_OK);
    fire(0);
    assert(heat_request);
    while (s_control.next_phase != PHASE_START) fire(0);
    fire(0);
    assert(!heat_request && (s_control.faults & HEATER_FAULT_TIMING));

    // Exercise the real notification loop over two complete frames.
    reset_recording();
    assert(heater_control_submit(&set) == ESP_OK);
    drive_task = true;
    if (setjmp(task_escape) == 0) control_task(NULL);
    drive_task = false;
    assert(sent_messages == 2);
    assert(last_message.data.sequence == 2 && last_message.data.cycle_start_us == 40000);
    assert(last_message.data.valid_mask == 15 && last_message.data.faults == 0);
    assert(memcmp(last_message.data.duty, set.duty, 4) == 0);

    // A START renewed by PING holds through 3001 notification-loop frames (>60 s).
    reset();
    const heater_command_t ping = {.kind = HEATER_COMMAND_PING};
    assert(heater_control_submit(&ping) == ESP_OK);
    assert(!heat_request && !s_control.command_active);
    const heater_command_t hold = {.kind = HEATER_COMMAND_START, .duty = {5, 5, 5, 5}};
    assert(heater_control_submit(&hold) == ESP_OK);
    task_frame_limit = 3001;
    check_hold = drive_task = true;
    if (setjmp(task_escape) == 0) control_task(NULL);
    check_hold = drive_task = false;
    assert(sent_messages == 3001 && last_message.data.cycle_start_us == 60020000);
    assert(last_message.data.deadline_misses == 0 && last_message.data.telemetry_drops == 0);
    assert(heater_control_submit(&ping) == ESP_OK);
    assert(s_control.command_active && memcmp(s_control.desired, hold.duty, 4) == 0);

    // A new SET replaces the held values; STOP cancels them, and PING cannot
    // restart heating even when measurements would permit it.
    const heater_command_t replacement = {.kind = HEATER_COMMAND_SET, .duty = {0, 5, 0, 5}};
    assert(heater_control_submit(&replacement) == ESP_OK);
    assert(s_control.command_active && memcmp(s_control.desired, replacement.duty, 4) == 0);
    assert(heater_control_submit(&stop) == ESP_OK);
    assert(!heat_request && !s_control.command_active);
    const uint8_t zero_duty[4] = {0};
    assert(memcmp(s_control.desired, zero_duty, 4) == 0);
    assert(heater_control_submit(&ping) == ESP_OK);
    assert(!heat_request && !s_control.command_active);
    fire(0); // Precharge with held-low PWM.
    fire(0); // Next frame applies STOP's zero duty.
    assert(!heat_request && memcmp(pwm, zero_duty, 4) == 0);
    // Lease expiry runs in the phase ISR even if the network tasks are blocked.
    reset_recording();
    assert(heater_control_submit(&set) == ESP_OK);
    const uint64_t expiry = s_control.lease_deadline_us;
    s_control.next_cycle_start_us = expiry;
    s_control.alarm.alarm_count = expiry;
    fire(0);
    assert(!s_control.command_active && !heat_request);
    assert(s_control.faults & HEATER_FAULT_HOST_TIMEOUT);
    assert(heater_control_submit(&ping) == ESP_OK && !s_control.command_active);

    // Maintenance acknowledgements execute the real task branch, then prohibit
    // GPIO phase activity, ADC starts and SET/PING until an explicit resume.
    reset_recording();
    assert(heater_control_submit(&set) == ESP_OK);
    fire(0);
    reach_conversion();
    assert(measure_request);
    assert(heater_control_pause(pdMS_TO_TICKS(2000)) == ESP_OK);
    assert(s_control.paused && !timer_running && !heat_request && !measure_request);
    assert(heater_control_submit(&set) == ESP_ERR_INVALID_STATE);
    assert(heater_control_submit(&ping) == ESP_ERR_INVALID_STATE);
    phase_alarm(s_control.timer, &(gptimer_alarm_event_data_t){0}, NULL);
    assert(!heat_request && !measure_request);
    assert(heater_control_pause(pdMS_TO_TICKS(2000)) == ESP_OK);
    fake_now += 2000000;
    assert(heater_control_resume() == ESP_OK);
    assert(!s_control.maintenance && timer_running && !s_control.command_active);
    assert(s_control.alarm.alarm_count > fake_now);
    fire(0);
    assert(!heat_request && memcmp(pwm, zero_duty, 4) == 0);

    // An acknowledgement timeout never grants permission to write flash.
    reset(); pause_timeout = true;
    assert(heater_control_pause(pdMS_TO_TICKS(2000)) == ESP_ERR_TIMEOUT);
    assert(s_control.maintenance && !s_control.paused && !heat_request && !measure_request);
    assert(!heater_control_is_paused() && heater_control_is_maintenance());
    assert(heater_control_resume() == ESP_ERR_INVALID_STATE);
    // A failed ADC initialization leaves no task/timer. It still permits a
    // verified output shutdown so web/OTA can recover the device without ADCs.
    reset();
    s_control.task = s_control.timer = NULL;
    assert(heater_control_pause(pdMS_TO_TICKS(1000)) == ESP_ERR_INVALID_STATE);
    s_control.board_ready = true;
    assert(heater_control_pause(pdMS_TO_TICKS(1000)) == ESP_OK);
    assert(heater_control_is_paused() && !heat_request && !measure_request);
    assert(heater_control_submit(&stop) == ESP_OK);
    assert(heater_control_submit(&set) == ESP_ERR_INVALID_STATE);
    assert(heater_control_resume() == ESP_ERR_INVALID_STATE);
    puts("Control timing, shutdown, ADC event and backpressure tests passed");
    return 0;
}
