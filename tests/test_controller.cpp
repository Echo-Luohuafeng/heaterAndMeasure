// 四路加热测量组件与接口的实现及验证。
#include <cassert>
#include <cmath>
#include <cstdio>
#include <initializer_list>

#include "heater_controller.h"
static controller_config_t configuration() {
  controller_config_t c{};
  c.algorithm = CONTROLLER_PID;
  // 热模型仿真使用较小实验限幅，验证饱和与回算。
  c.max_duty = 2;
  c.tolerance_ohm = .05f;
  c.parameters[12] = .1f;
  c.parameters[13] = 1;
  for (unsigned i = 0; i < 4; ++i) {
    c.target[i] = 110;
    c.parameters[i] = .5f;
    c.parameters[4 + i] = .05f;
  }
  return c;
}
// 不同实验限幅采用相同的量化规则，覆盖半百分点及接口上界。
static void TestDutyRange() {
  for (float limit : {3.5f, 50.0f, 95.0f}) {
    auto config = configuration();
    config.max_duty = limit;
    for (unsigned i = 0; i < 4; ++i) config.parameters[i] = 100;
    assert(heater_controller_configure(&config));
    controller_input_t input{};
    input.sequence = 1;
    input.sample_time_us = 20000;
    input.valid_mask = 15;
    for (float& r : input.resistance) r = 100;
    const auto output = heater_controller_step(&input);
    assert(output.valid && output.limited_mask == 15);
    for (auto ticks : output.ticks) assert(ticks == limit * 2);
  }
  auto invalid = configuration();
  invalid.max_duty = 95.5f;
  assert(!heater_controller_validate(&invalid));
}

int main() {
  TestDutyRange();
  auto c = configuration();
  assert(heater_controller_configure(&c));
  auto invalid = c;
  invalid.parameters[0] = NAN;
  assert(!heater_controller_validate(&invalid));
  invalid = c;
  invalid.algorithm = 255;
  assert(!heater_controller_validate(&invalid));
  controller_input_t in{};
  in.valid_mask = 15;
  in.sample_time_us = 20000;
  in.sequence = 1;
  for (float& r : in.resistance) r = 100;
  auto out = heater_controller_step(&in);
  assert(out.valid && out.limited_mask == 15);
  for (auto t : out.ticks) assert(t == 4);
  in.sequence = 2;
  in.sample_time_us += 20000;
  in.valid_mask = 14;
  in.resistance[0] = NAN;
  out = heater_controller_step(&in);
  assert(out.valid && out.ticks[0] == 0);
  in.sequence = 3;
  in.sample_time_us += 20000;
  in.valid_mask = 15;
  in.resistance[0] = 100;
  out = heater_controller_step(&in);
  assert(out.valid);
  auto duplicate = heater_controller_step(&in);
  assert(!duplicate.valid);
  heater_controller_reset();
  float r = 100;
  float delay[2] = {0, 0};
  float error_sum = 0;
  for (unsigned k = 1; k <= 6000; ++k) {
    in.sequence = k;
    in.sample_time_us = k * 20000ULL;
    in.valid_mask = 15;
    for (float& x : in.resistance) x = r;
    out = heater_controller_step(&in);
    assert(out.valid);
    for (auto t : out.ticks) assert(t <= 4);
    const float applied = delay[k % 2];
    delay[k % 2] = out.ticks[0] * .5f;
    r += (100 + 10 * applied - r) * .02f / 8;
    if (k > 5000) error_sum += std::abs(110 - r);
  }
  assert(error_sum / 1000 < .5f);
  // A large downward setpoint and reset immediately inhibit heating.
  c.target[0] = 90;
  assert(heater_controller_configure(&c));
  out = heater_controller_step(&in);
  assert(out.valid && out.ticks[0] == 0);
  c = configuration();
  for (float& t : c.target) t = 100;
  assert(heater_controller_configure(&c));
  in.sequence = 1;
  in.sample_time_us = 20000;
  for (float& value : in.resistance) value = 100;
  out = heater_controller_step(&in);
  assert(out.valid);
  controller_applied_t feedback{};
  feedback.source_sequence = 1;
  feedback.requested[0] = .2f;
  feedback.applied[0] = 0;
  heater_controller_on_applied(&feedback);
  in.sequence = 2;
  in.sample_time_us = 40000;
  out = heater_controller_step(&in);
  assert(std::abs(out.requested[0]) <
         1e-6f);  // Quantization must not cancel integration.
  feedback.blocked_mask = 1;
  heater_controller_on_applied(&feedback);
  in.sequence = 3;
  in.sample_time_us = 60000;
  out = heater_controller_step(&in);
  assert(out.requested[0] < 0 &&
         out.ticks[0] == 0);  // Safety blocking does back-calculate.
  std::printf(
      "PID limits, invalid samples, reset, timestamps and delayed thermal "
      "simulation passed; MAE %.4f ohm\n",
      error_sum / 1000);
}
