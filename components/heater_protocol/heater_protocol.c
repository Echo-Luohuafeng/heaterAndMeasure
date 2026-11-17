#include "heater_protocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r';
}

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
    if (length == 4 && memcmp(line, "STOP", 4) == 0) {
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
                // Bound every step, so arbitrarily long numbers cannot overflow.
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

int32_t heater_decode_adc24(const uint8_t bytes[3])
{
    const uint32_t bits = ((uint32_t)bytes[0] << 16) | ((uint32_t)bytes[1] << 8) | bytes[2];
    return (int32_t)bits - ((bits & 0x800000U) ? 0x1000000 : 0);
}

float heater_resistance_ohm(int32_t code, uint32_t reference_mohm, int32_t offset_mohm)
{
    // 1 LSB = Vref / (gain * 2^23); the excitation current cancels.
    return (float)((double)code * reference_mohm / (8388608.0 * HEATER_ADC_GAIN * 1000.0)
                   + offset_mohm / 1000.0);
}

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
