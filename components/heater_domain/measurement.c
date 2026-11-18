// 纯测量规则可在 Linux 上验证，不访问外设。
#include <math.h>

#include "heater_domain.h"
// 先拼接 24 位无符号值，再按最高位减去 2^24 实现补码符号扩展。
int32_t heater_decode_adc24(const uint8_t bytes[3]) {
  const uint32_t bits =
      ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
  return (int32_t)bits - ((bits & 0x800000U) ? 0x1000000 : 0);
}

// 按比例式 R = code × Rref / (4 × 2^23) + offset 换算并将 mΩ 转为 Ω。
// 中间计算使用 double，以降低原始码乘参考电阻时的舍入误差。
float heater_resistance_ohm(int32_t code, uint32_t reference_mohm,
                            int32_t offset_mohm) {
  // 1 LSB = Vref / (gain × 2^23)，比例测量中的激励电流相消。
  return (float)((double)code * reference_mohm /
                     (8388608.0 * HEATER_ADC_GAIN * 1000.0) +
                 offset_mohm / 1000.0);
}

bool heater_sample_valid(int32_t code, float resistance, float minimum,
                         float maximum) {
  return code > 0 && code < 0x7fffff && isfinite(resistance) &&
         resistance >= minimum && resistance <= maximum;
}
bool heater_duties_valid(const uint8_t duty[4], unsigned limit) {
  if (!duty || limit > HEATER_MAX_DUTY) return false;
  for (unsigned i = 0; i < 4; ++i)
    if (duty[i] > limit) return false;
  return true;
}
