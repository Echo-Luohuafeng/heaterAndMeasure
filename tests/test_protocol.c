#include "heater_protocol.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    heater_command_t started;
    assert(heater_command_parse("START 0 1 2 95", 14, &started));
    assert(started.kind == HEATER_COMMAND_START && started.duty[3] == 95);
    assert(!heater_command_parse("START 0 1 2", 11, &started));
    assert(!heater_command_parse("START 0 1 2 96", 14, &started));
    heater_command_t command = {.kind = HEATER_COMMAND_SET, .duty = {1, 2, 3, 4}};
    const char *valid[] = {"SET 0 95 10 20", " \tSET\t1 2 3 4\r", "STOP", " PING\r"};
    for (size_t i = 0; i < sizeof(valid) / sizeof(valid[0]); ++i) {
        assert(heater_command_parse(valid[i], strlen(valid[i]), &command));
    }
    const char *invalid[] = {
        "SET 96 0 0 0", "SET -1 0 0 0", "SET 1.0 0 0 0", "SET 1 2 3",
        "SET 1 2 3 4 5", "SET 1 2 3 4x", "SET 1,2,3,4", "STOP 1",
        "SET 4294967296 0 0 0", "SET 1 2 3 +4", "", "SET1 2 3 4",
    };
    command = (heater_command_t){.kind = HEATER_COMMAND_SET, .duty = {11, 22, 33, 44}};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        const heater_command_t before = command;
        assert(!heater_command_parse(invalid[i], strlen(invalid[i]), &command));
        assert(memcmp(&before, &command, sizeof(command)) == 0);
    }
    const uint8_t zero[] = {0, 0, 0};
    const uint8_t positive[] = {0x7f, 0xff, 0xff};
    const uint8_t negative[] = {0x80, 0, 0};
    const uint8_t minus_one[] = {0xff, 0xff, 0xff};
    assert(heater_decode_adc24(zero) == 0);
    assert(heater_decode_adc24(positive) == 8388607);
    assert(heater_decode_adc24(negative) == -8388608);
    assert(heater_decode_adc24(minus_one) == -1);
    assert(fabsf(heater_resistance_ohm(3355443, 1000000, 0) - 100.0f) < 0.00004f);
    assert(fabsf(heater_resistance_ohm(5368709, 1000000, 0) - 160.0f) < 0.00004f);
    assert(fabsf(heater_resistance_ohm(3355443, 1001000, -50) - 100.05f) < 0.00004f);
    const heater_telemetry_t data = {
        .sequence = 42, .cycle_start_us = 860000, .duty = {0, 1, 50, 95},
        .resistance_ohm = {100, 120, 160, NAN}, .valid_mask = 7,
    };
    char line[HEATER_LINE_MAX_LENGTH];
    assert(heater_telemetry_format(&data, line, sizeof(line)) > 0);
    assert(strstr(line, "DATA,42,860000,0,1,50,95,100.000000,120.000000,160.000000,nan,7,") == line);
    assert(line[strlen(line) - 1] == '\n');
    assert(heater_telemetry_format(&data, line, 8) == -1);
    puts("Protocol and ADC numerical tests passed");
    return 0;
}
