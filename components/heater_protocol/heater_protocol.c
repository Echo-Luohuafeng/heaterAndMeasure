#include "heater_protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

// 判断 `c` 是否为命令允许的空格、制表符或回车，换行由通信层分帧处理。
static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

// 先裁剪两端空白，再按完整命令及四路整数解析，逐位检查占空比上限。
// 使用局部结果，只有所有字符都合法时才写回输出，避免失败污染已有设定。
bool heater_command_parse(const char *line, size_t length, heater_command_t *command)
{
    if (line == NULL || command == NULL || length == 0 || length > HEATER_COMMAND_MAX_LENGTH) {
        return false;
    }
    while (length > 0 && is_space(*line)) {
        ++line;
        --length;
    }
    while (length > 0 && is_space(line[length - 1])) {
        --length;
    }
    heater_command_t parsed = {0};
    if (length == 6 && memcmp(line, "RSTART", 6) == 0) {
        parsed.kind = HEATER_COMMAND_RESISTANCE_START;
    } else if (length == 4 && memcmp(line, "STOP", 4) == 0) {
        parsed.kind = HEATER_COMMAND_STOP;
    } else if (length == 4 && memcmp(line, "PING", 4) == 0) {
        parsed.kind = HEATER_COMMAND_PING;
    } else {
        size_t position;
        if (length >= 4 && memcmp(line, "SET", 3) == 0 && is_space(line[3])) {
            parsed.kind = HEATER_COMMAND_SET;
            position = 3;
        } else if (length >= 6 && memcmp(line, "START", 5) == 0 && is_space(line[5])) {
            parsed.kind = HEATER_COMMAND_START;
            position = 5;
        } else return false;
        for (size_t channel = 0; channel < HEATER_CHANNEL_COUNT; ++channel) {
            if (position >= length || !is_space(line[position])) {
                return false;
            }
            while (position < length && is_space(line[position])) {
                ++position;
            }
            if (position >= length || line[position] < '0' || line[position] > '9') {
                return false;
            }
            unsigned value = 0;
            while (position < length && line[position] >= '0' && line[position] <= '9') {
                value = value * 10U + (unsigned)(line[position++] - '0');
                // 每解析一位都检查上限，避免超长数字造成整数溢出。
                if (value > HEATER_MAX_DUTY) {
                    return false;
                }
            }
            parsed.duty[channel] = (uint8_t)value;
        }
        if (position != length) {
            return false;
        }
    }
    *command = parsed;
    return true;
}

// 先拼接 24 位无符号值，再按最高位减去 2^24 实现补码符号扩展。
int32_t heater_decode_adc24(const uint8_t bytes[3])
{
    const uint32_t bits = ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
    return (int32_t)bits - ((bits & 0x800000U) ? 0x1000000 : 0);
}

// 按比例式 R = code × Rref / (4 × 2^23) + offset 换算并将 mΩ 转为 Ω。
// 中间计算使用 double，以降低原始码乘参考电阻时的舍入误差。
float heater_resistance_ohm(int32_t code, uint32_t reference_mohm, int32_t offset_mohm)
{
    // 1 LSB = Vref / (gain × 2^23)，比例测量中的激励电流相消。
    return (float)((double)code * reference_mohm / (8388608.0 * HEATER_ADC_GAIN * 1000.0)
                   + offset_mohm / 1000.0);
}

// 按旧 DATA 字段顺序输出一帧，用 snprintf 返回值检测格式化失败及截断。
int heater_telemetry_format(const heater_telemetry_t *data, char *output, size_t capacity)
{
    const int length = snprintf(output, capacity,
        "DATA,%" PRIu32 ",%" PRIu64 ",%u,%u,%u,%u,%.6f,%.6f,%.6f,%.6f,%u,%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "\n",
        data->sequence, data->cycle_start_us,
        data->duty[0], data->duty[1], data->duty[2], data->duty[3],
        (double)data->resistance_ohm[0], (double)data->resistance_ohm[1],
        (double)data->resistance_ohm[2], (double)data->resistance_ohm[3],
        data->valid_mask, data->faults, data->deadline_misses, data->telemetry_drops,
        data->control_stack_free_bytes);
    return length >= 0 && (size_t)length < capacity ? length : -1;
}
