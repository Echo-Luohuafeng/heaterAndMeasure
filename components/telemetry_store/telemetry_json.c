#include "telemetry_json.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

int telemetry_record_json(const telemetry_record_t *record, char *output, size_t capacity)
{
    const heater_telemetry_t *f = &record->frame;
    char resistance[4][32];
    for (size_t i = 0; i < 4; ++i) {
        if ((f->valid_mask & (1U << i)) && isfinite(f->resistance_ohm[i])) {
            snprintf(resistance[i], sizeof(resistance[i]), "%.6f", (double)f->resistance_ohm[i]);
        } else {
            strcpy(resistance[i], "null");
        }
    }
    const int length = snprintf(output, capacity,
        "[%" PRIu64 ",%" PRIu32 ",%" PRIu64 ",[%u,%u,%u,%u],[%s,%s,%s,%s],%u,%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 "]",
        record->cursor, f->sequence, f->cycle_start_us,
        f->duty[0], f->duty[1], f->duty[2], f->duty[3],
        resistance[0], resistance[1], resistance[2], resistance[3], f->valid_mask,
        f->faults, f->deadline_misses, f->telemetry_drops, f->control_stack_free_bytes);
    return length < 0 || (size_t)length >= capacity ? -1 : length;
}

bool telemetry_parse_cursor(const char *text, uint64_t *value)
{
    if (!text || !*text) return false;
    uint64_t parsed = 0;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        const unsigned digit = (unsigned)(*text - '0');
        if (parsed > (UINT64_MAX - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    *value = parsed;
    return true;
}

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool telemetry_form_value(const char *body, const char *key, char *value, size_t capacity)
{
    if (!capacity) return false;
    bool found = false;
    const size_t key_size = strlen(key);
    for (const char *part = body; *part;) {
        const char *end = strchr(part, '&');
        if (!end) end = part + strlen(part);
        const char *equal = memchr(part, '=', (size_t)(end - part));
        if (equal && (size_t)(equal - part) == key_size && !memcmp(part, key, key_size)) {
            if (found) return false; // reject ambiguous duplicate fields
            found = true;
            size_t used = 0;
            for (const char *p = equal + 1; p < end; ++p) {
                unsigned char c = (unsigned char)*p;
                if (c == '%') {
                    if (end - p < 3) return false;
                    const int hi = hex_digit(p[1]), lo = hex_digit(p[2]);
                    if (hi < 0 || lo < 0) return false;
                    c = (unsigned char)(hi * 16 + lo);
                    p += 2;
                } else if (c == '+') c = ' ';
                if (!c || c < 32 || c == 127 || used + 1 >= capacity) return false;
                value[used++] = (char)c;
            }
            value[used] = '\0';
        }
        part = *end ? end + 1 : end;
    }
    return found;
}
