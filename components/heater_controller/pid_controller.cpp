#include <algorithm>
#include <cmath>

#include "controller.hpp"

namespace heater {
bool pid_valid(const controller_config_t& c) noexcept {
  if (!std::isfinite(c.parameters[12]) || c.parameters[12] <= 0 ||
      c.parameters[12] > 60 || !std::isfinite(c.parameters[13]) ||
      c.parameters[13] < 0 || c.parameters[13] > 50 ||
      !std::isfinite(c.tolerance_ohm) || c.tolerance_ohm <= 0 ||
      c.tolerance_ohm > 10)
    return false;
  for (unsigned i = 0; i < 4; ++i) {
    if (!std::isfinite(c.parameters[i]) || c.parameters[i] < 0 ||
        c.parameters[i] > 100 || !std::isfinite(c.parameters[4 + i]) ||
        c.parameters[4 + i] < 0 || c.parameters[4 + i] > 100 ||
        !std::isfinite(c.parameters[8 + i]) || c.parameters[8 + i] < 0 ||
        c.parameters[8 + i] > 100)
      return false;
  }
  return true;
}

class PidController final : public Controller {
  controller_config_t config_{};
  struct State {
    float integral{}, measurement{}, derivative{};
    bool previous_valid{};
  } state_[4];
  uint64_t previous_time_{};

 public:
  bool Configure(const controller_config_t& c) noexcept override {
    if (!pid_valid(c)) return false;
    config_ = c;
    Reset();
    return true;
  }
  void Reset() noexcept override {
    for (auto& s : state_) s = {};
    previous_time_ = 0;
  }
  void OnApplied(const controller_applied_t& a) noexcept override {
    if (!a.source_sequence) return;
    for (unsigned i = 0; i < 4; ++i) {
      if (!std::isfinite(a.requested[i]) || !std::isfinite(a.applied[i]))
        continue;
      if (!((a.blocked_mask | a.limited_mask) & (1U << i))) continue;
      // 普通量化误差不回算积分，避免把稳态补偿钳在某个 PWM 档位。
      // 以对应来源的实际执行差异回算积分，包含限幅、量化和安全禁止。
      state_[i].integral +=
          config_.parameters[13] * 0.02f * (a.applied[i] - a.requested[i]);
      state_[i].integral =
          std::clamp(state_[i].integral, -config_.max_duty, config_.max_duty);
    }
  }
  controller_output_t Step(const controller_input_t& in) noexcept override {
    controller_output_t out{};
    out.source_sequence = in.sequence;
    float dt = 0.02f;
    if (previous_time_) {
      if (in.sample_time_us <= previous_time_) return out;
      dt = static_cast<float>(in.sample_time_us - previous_time_) * 1e-6f;
      if (dt > 0.1f) {
        Reset();
        return out;
      }
    }
    previous_time_ = in.sample_time_us;
    for (unsigned i = 0; i < 4; ++i) {
      auto& s = state_[i];
      if (!(in.valid_mask & (1U << i)) || !std::isfinite(in.resistance[i])) {
        s.previous_valid = false;
        s.derivative = 0;
        continue;
      }
      const float error = config_.target[i] - in.resistance[i];
      const float derivative =
          s.previous_valid ? -(in.resistance[i] - s.measurement) / dt : 0;
      s.derivative +=
          dt / (config_.parameters[12] + dt) * (derivative - s.derivative);
      s.measurement = in.resistance[i];
      s.previous_valid = true;
      const float pd = config_.parameters[i] * error +
                       config_.parameters[8 + i] * s.derivative;
      const float trial = s.integral + config_.parameters[4 + i] * error * dt;
      const float trial_output = pd + trial;
      // 饱和时只允许朝解除饱和方向积分。
      if (!((trial_output > config_.max_duty && error > 0) ||
            (trial_output < 0 && error < 0)))
        s.integral = trial;
      out.requested[i] = pd + s.integral;
      if (!std::isfinite(out.requested[i])) return controller_output_t{};
    }
    out.valid = true;
    return out;
  }
};
static PidController pid;
Controller& pid_instance() noexcept { return pid; }
}  // namespace heater
