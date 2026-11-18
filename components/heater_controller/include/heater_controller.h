#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define CONTROLLER_CHANNELS 4
#define CONTROLLER_DELAY_CYCLES 2
#define CONTROLLER_PID 1
// 占空比单位为加热窗口内的百分比；算法接口不依赖 ESP-IDF，可在主机仿真。
typedef struct {
    uint32_t algorithm;
    float target[4];
    float parameters[32]; // 注册表声明每个参数在此数组中的位置。
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
    uint8_t ticks[4]; // 每个计数对应 0.5 个百分点。
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
    const char *name;
    unsigned offset, count;
    float minimum, maximum, default_value;
} controller_parameter_t;
typedef struct {
    uint32_t id;
    const char *name;
    const char *version;
    const controller_parameter_t *parameters;
    unsigned parameter_count;
} controller_descriptor_t;
const controller_descriptor_t *heater_controller_registry(unsigned *count);
bool heater_controller_validate(const controller_config_t *config);
// 仅由算法任务调用；configure/reset 不在 ISR 中执行。
bool heater_controller_configure(const controller_config_t *config);
void heater_controller_reset(void);
void heater_controller_on_applied(const controller_applied_t *applied);
controller_output_t heater_controller_step(const controller_input_t *input);
#ifdef __cplusplus
}
#endif
