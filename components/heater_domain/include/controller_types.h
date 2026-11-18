// 算法值类型不依赖操作系统，支持主机仿真。
#pragma once
#include <stdbool.h>
#include <stdint.h>
#define CONTROLLER_CHANNELS 4
#define CONTROLLER_DELAY_CYCLES 2
#define CONTROLLER_PID 1
// 占空比单位为加热窗口内的百分比；算法接口不依赖 ESP-IDF，可在主机仿真。
typedef struct {
  uint32_t algorithm;
  float target[4];
  float parameters[32];  // 注册表声明每个参数在此数组中的位置。
  float tolerance_ohm;
  float max_duty;
} controller_config_t;
typedef struct {
  uint32_t sequence;
  uint64_t sample_time_us;
  float resistance[4];
  uint8_t valid_mask;
} controller_input_t;
typedef struct {
  uint32_t source_sequence;
  float requested[4];
  uint8_t ticks[4];  // 每个计数对应 0.5 个百分点。
  uint8_t limited_mask;
  bool valid;
} controller_output_t;
typedef struct {
  uint32_t source_sequence;
  float requested[4];
  float applied[4];
  uint8_t blocked_mask;
  uint8_t limited_mask;
} controller_applied_t;
typedef struct {
  const char* name;
  unsigned offset, count;
  float minimum, maximum, default_value;
} controller_parameter_t;
typedef struct {
  uint32_t id;
  const char* name;
  const char* version;
  const controller_parameter_t* parameters;
  unsigned parameter_count;
} controller_descriptor_t;
