// 四路加热测量组件与接口的实现及验证。
#include <cassert>
#include <cmath>
#include <cstdio>

#include "controller.hpp"
namespace heater {
class TestController final : public Controller {
 public:
  bool Configure(const controller_config_t&) noexcept override { return true; }
  void Reset() noexcept override {}
  controller_output_t Step(const controller_input_t& in) noexcept override {
    controller_output_t out{};
    out.valid = true;
    out.source_sequence = in.sequence;
    out.requested[0] = 3.2f;
    out.requested[1] = 1.2f;
    out.requested[2] = -1;
    if (in.sequence == 2) out.requested[3] = NAN;
    if (in.sequence == 3) ++out.source_sequence;
    return out;  // No PWM handling in the subclass.
  }
};
static TestController test;
static Controller& instance() noexcept { return test; }
static bool validate(const controller_config_t&) noexcept { return true; }
const Registration* registrations(unsigned& count) noexcept {
  static const controller_parameter_t params[] = {{"gain", 0, 1, 0, 10, 1}};
  static const Registration table[] = {
      {{99, "test", "1", params, 1}, instance, validate}};
  count = 1;
  return table;
}
}  // namespace heater
int main() {
  controller_config_t config{};
  config.algorithm = 99;
  config.max_duty = 2;
  config.parameters[0] = 1;
  assert(heater_controller_configure(&config));
  controller_input_t in{};
  in.sequence = 1;
  in.valid_mask = 15;
  auto out = heater_controller_step(&in);
  assert(out.valid);
  assert(out.ticks[0] == 4 && out.ticks[1] == 2 && out.ticks[2] == 0);
  assert(out.limited_mask == 5);
  in.sequence = 2;
  out = heater_controller_step(&in);
  assert(!out.valid);
  for (auto t : out.ticks) assert(t == 0);
  in.sequence = 3;
  assert(!heater_controller_step(&in).valid);
  in.sequence = 4;
  in.valid_mask = 14;
  out = heater_controller_step(&in);
  assert(out.valid && out.ticks[0] == 0);
  puts(
      "Alternate subclass reuses registry, common limiting/quantization and "
      "invalid-output rejection");
}
