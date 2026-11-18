#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "controller_types.h"
const controller_descriptor_t* heater_controller_registry(unsigned* count);
bool heater_controller_validate(const controller_config_t* config);
// 仅由算法任务调用；configure/reset 不在 ISR 中执行。
bool heater_controller_configure(const controller_config_t* config);
void heater_controller_reset(void);
void heater_controller_on_applied(const controller_applied_t* applied);
controller_output_t heater_controller_step(const controller_input_t* input);
#ifdef __cplusplus
}
#endif
