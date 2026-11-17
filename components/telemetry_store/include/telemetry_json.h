#pragma once
#include "telemetry_ring.h"

// Compact schema: [cursor, sequence, time_us, duty[4], resistance[4], valid_mask,
// faults, deadline_misses, telemetry_drops, stack_free_bytes]. Invalid R is null.
int telemetry_record_json(const telemetry_record_t *record, char *output, size_t capacity);
bool telemetry_parse_cursor(const char *text, uint64_t *value);
bool telemetry_form_value(const char *body, const char *key, char *value, size_t capacity);
