#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HEATER_CHANNEL_COUNT 4
#define HEATER_MAX_DUTY 95
#define HEATER_COMMAND_MAX_LENGTH 64
#define HEATER_LINE_MAX_LENGTH 256
#define HEATER_PERIOD_US 20000U
#define HEATER_ADC_GAIN 4U

typedef enum {
    HEATER_COMMAND_SET,
    HEATER_COMMAND_STOP,
    HEATER_COMMAND_PING,
} heater_command_kind_t;

typedef struct {
    heater_command_kind_t kind;
    uint8_t duty[HEATER_CHANNEL_COUNT];
} heater_command_t;

enum {
    HEATER_FAULT_TIMING = 1U << 0,
    HEATER_FAULT_ADC_TIMEOUT = 1U << 1,
    HEATER_FAULT_SPI = 1U << 2,
    HEATER_FAULT_SAMPLE = 1U << 3,
    HEATER_FAULT_HOST_TIMEOUT = 1U << 4,
    HEATER_FAULT_STOPPED = 1U << 5,
    HEATER_FAULT_PWM = 1U << 6,
};

typedef struct {
    uint32_t sequence;
    uint64_t cycle_start_us;
    uint8_t duty[HEATER_CHANNEL_COUNT];
    float resistance_ohm[HEATER_CHANNEL_COUNT];
    uint8_t valid_mask;
    uint32_t faults;
    uint32_t deadline_misses;
    uint32_t telemetry_drops;
    uint32_t control_stack_free_bytes;
} heater_telemetry_t;

typedef enum {
    HEATER_MESSAGE_DATA,
    HEATER_MESSAGE_REPLY,
} heater_message_kind_t;

typedef struct {
    heater_message_kind_t kind;
    union {
        heater_telemetry_t data;
        char reply[HEATER_COMMAND_MAX_LENGTH];
    };
} heater_message_t;

// Accept exactly SET d1 d2 d3 d4, STOP, or PING. No partial command is applied.
bool heater_command_parse(const char *line, size_t length, heater_command_t *command);
int32_t heater_decode_adc24(const uint8_t bytes[3]);
float heater_resistance_ohm(int32_t code, uint32_t reference_mohm, int32_t offset_mohm);
// Returns the full formatted length, or -1 if the output does not fit.
int heater_telemetry_format(const heater_telemetry_t *data, char *output, size_t capacity);
