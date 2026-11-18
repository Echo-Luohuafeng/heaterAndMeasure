// 算法注册表是新增算法的唯一入口；实例具有静态生命周期。
#include "controller.hpp"
namespace heater {
Controller& pid_instance() noexcept;
bool pid_valid(const controller_config_t&) noexcept;
const Registration* registrations(unsigned& count) noexcept {
  static const controller_parameter_t params[] = {
      {"kp", 0, 4, 0, 100, 0.5f},
      {"ki", 4, 4, 0, 100, 0.05f},
      {"kd", 8, 4, 0, 100, 0},
      {"derivative_tau_s", 12, 1, 0.001f, 60, 0.1f},
      {"tracking_gain", 13, 1, 0, 50, 1},
  };
  static const Registration table[] = {
      {{CONTROLLER_PID, "pid", "1", params, 5}, pid_instance, pid_valid}};
  count = sizeof(table) / sizeof(table[0]);
  return table;
}
}  // namespace heater
