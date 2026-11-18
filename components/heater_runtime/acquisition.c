// 保留已验证的时序；所有硬件操作属于实时执行组件。
#include <math.h>

#include "ads1220.h"
#include "runtime_internal.h"
static const uint32_t s_reference_mohm[HEATER_CHANNEL_COUNT] = {
    CONFIG_HEATER_REFERENCE_1_MOHM,
    CONFIG_HEATER_REFERENCE_2_MOHM,
    CONFIG_HEATER_REFERENCE_3_MOHM,
    CONFIG_HEATER_REFERENCE_4_MOHM,
};
static const int32_t s_offset_mohm[HEATER_CHANNEL_COUNT] = {
    CONFIG_HEATER_OFFSET_1_MOHM,
    CONFIG_HEATER_OFFSET_2_MOHM,
    CONFIG_HEATER_OFFSET_3_MOHM,
    CONFIG_HEATER_OFFSET_4_MOHM,
};
// 锁存转换窗口内的 DRDY 下降沿，仅接收已启动且满足最早就绪时间的通道。
// `arg` 指向常驻内部 RAM 的通道索引；ISR 仅标记就绪并通知任务读取。
void IRAM_ATTR adc_ready(void* arg) {
  const size_t channel = *(uint8_t*)arg;
  const uint8_t bit = 1U << channel;
  uint64_t now;
  if (gptimer_get_raw_count(s_runtime.timer, &now) != ESP_OK) {
    return;
  }
  bool notify = false;
  portENTER_CRITICAL_ISR(&s_runtime.lock);
  if (!(s_runtime.device_state == HEATER_DEVICE_PAUSING ||
        s_runtime.device_state == HEATER_DEVICE_PAUSED) &&
      s_runtime.phase == PHASE_CONVERT && (s_runtime.started_mask & bit) &&
      now >= s_runtime.adc_start_us[channel] + ADC_EARLIEST_READY_US &&
      now < s_runtime.cycle_start_us + MEASURE_OFF_US) {
    // 分别锁存各通道下降沿，避免共享 SPI 时钟清除其他 ADC 的 DRDY
    // 后丢失就绪状态。
    s_runtime.ready_mask |= bit;
    notify = true;
  }
  portEXIT_CRITICAL_ISR(&s_runtime.lock);
  if (notify) {
    BaseType_t woken = pdFALSE;
    xTaskNotifyFromISR(s_runtime.task, EVENT_ADC_READY, eSetBits, &woken);
    if (woken) {
      portYIELD_FROM_ISR();
    }
  }
}

// 在任务中依次启动四路 ADC，每条命令前后检查阶段、周期及 17.10 ms 截止。
// 只记录 `sequence` 的故障；提前登记启动时间供 DRDY ISR 检查，失败撤销许可。
void start_conversion(uint32_t sequence) {
  for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
    uint64_t now;
    if (gptimer_get_raw_count(s_runtime.timer, &now) != ESP_OK) {
      runtime_add_fault(sequence, HEATER_FAULT_TIMING);
      return;
    }
    portENTER_CRITICAL(&s_runtime.lock);
    const bool on_time =
        !(s_runtime.device_state == HEATER_DEVICE_PAUSING ||
          s_runtime.device_state == HEATER_DEVICE_PAUSED) &&
        s_runtime.sequence == sequence && s_runtime.phase == PHASE_CONVERT &&
        !s_runtime.pwm_failed &&
        now <= s_runtime.cycle_start_us + ADC_START_DEADLINE_US;
    if (on_time) {
      s_runtime.adc_start_us[i] = now;
      s_runtime.started_mask |= 1U << i;
    }
    portEXIT_CRITICAL(&s_runtime.lock);
    if (!on_time) {
      portENTER_CRITICAL(&s_runtime.lock);
      ++s_runtime.timing.adc_start_misses;
      const uint32_t offset = now > s_runtime.cycle_start_us
                                  ? (uint32_t)(now - s_runtime.cycle_start_us)
                                  : 0;
      if (offset > s_runtime.timing.adc_start_max_offset_us)
        s_runtime.timing.adc_start_max_offset_us = offset;
      portEXIT_CRITICAL(&s_runtime.lock);
      runtime_add_fault(sequence, HEATER_FAULT_TIMING);
      return;
    }
    if (ads1220_start(i) != ESP_OK) {
      portENTER_CRITICAL(&s_runtime.lock);
      s_runtime.started_mask &= ~(1U << i);
      portEXIT_CRITICAL(&s_runtime.lock);
      runtime_add_fault(sequence, HEATER_FAULT_SPI);
      continue;
    }
    if (gptimer_get_raw_count(s_runtime.timer, &now) != ESP_OK) {
      runtime_add_fault(sequence, HEATER_FAULT_TIMING);
      return;
    }
    portENTER_CRITICAL(&s_runtime.lock);
    const bool completed_on_time =
        !(s_runtime.device_state == HEATER_DEVICE_PAUSING ||
          s_runtime.device_state == HEATER_DEVICE_PAUSED) &&
        s_runtime.sequence == sequence && s_runtime.phase == PHASE_CONVERT &&
        now <= s_runtime.cycle_start_us + ADC_START_DEADLINE_US;
    if (s_runtime.sequence == sequence) {
      const uint32_t offset = (uint32_t)(now - s_runtime.cycle_start_us);
      if (offset > s_runtime.timing.adc_start_max_offset_us)
        s_runtime.timing.adc_start_max_offset_us = offset;
      if (!completed_on_time) ++s_runtime.timing.adc_start_misses;
    }
    if (!completed_on_time && s_runtime.sequence == sequence) {
      s_runtime.started_mask &= ~(1U << i);
    }
    portEXIT_CRITICAL(&s_runtime.lock);
    if (!completed_on_time) {
      runtime_add_fault(sequence, HEATER_FAULT_TIMING);
      return;
    }
  }
}

// 读取 `frame` 当前周期已锁存且未读的通道，更新电阻和有效掩码。
// 非空的 `read_mask` 按通道记录已尝试读取，避免 SPI 失败后重复读取。
// SPI 前预留截止裕量，返回后再次检查截止；饱和或越界电阻不获得通道许可。
void read_ready(heater_telemetry_t* frame, uint8_t* read_mask) {
  const snapshot_t current = runtime_snapshot();
  if (current.sequence != frame->sequence || current.phase != PHASE_CONVERT) {
    return;
  }
  for (size_t i = 0; i < HEATER_CHANNEL_COUNT; ++i) {
    const uint8_t bit = 1U << i;
    if (!(current.ready_mask & bit) || (*read_mask & bit)) {
      continue;
    }
    uint64_t now;
    if (gptimer_get_raw_count(s_runtime.timer, &now) != ESP_OK ||
        now + ADC_READ_GUARD_US >= frame->cycle_start_us + MEASURE_OFF_US) {
      portENTER_CRITICAL(&s_runtime.lock);
      ++s_runtime.timing.adc_read_misses;
      portEXIT_CRITICAL(&s_runtime.lock);
      runtime_add_fault(frame->sequence, HEATER_FAULT_TIMING);
      return;
    }
    int32_t code;
    *read_mask |= bit;
    if (ads1220_read(i, &code) != ESP_OK) {
      runtime_add_fault(frame->sequence, HEATER_FAULT_SPI);
      continue;
    }
    if (gptimer_get_raw_count(s_runtime.timer, &now) != ESP_OK ||
        now >= frame->cycle_start_us + MEASURE_OFF_US) {
      portENTER_CRITICAL(&s_runtime.lock);
      ++s_runtime.timing.adc_read_misses;
      portEXIT_CRITICAL(&s_runtime.lock);
      runtime_add_fault(frame->sequence, HEATER_FAULT_TIMING);
      continue;
    }
    const float resistance =
        heater_resistance_ohm(code, s_reference_mohm[i], s_offset_mohm[i]);
    if (!heater_sample_valid(code, resistance,
                             CONFIG_HEATER_VALID_MIN_MOHM / 1000.0f,
                             CONFIG_HEATER_VALID_MAX_MOHM / 1000.0f)) {
      runtime_add_fault(frame->sequence, HEATER_FAULT_SAMPLE);
      continue;
    }
    frame->resistance_ohm[i] = resistance;
    frame->valid_mask |= bit;
  }
}
