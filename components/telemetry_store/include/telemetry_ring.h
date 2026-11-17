#pragma once

#include "heater_protocol.h"

typedef struct {
    uint64_t cursor;
    heater_telemetry_t frame;
} telemetry_record_t;

typedef struct {
    telemetry_record_t *records;
    size_t capacity;
    uint64_t next;
} telemetry_ring_t;

typedef struct {
    uint64_t oldest;
    uint64_t next;
    uint64_t overwritten;
    size_t capacity;
} telemetry_stats_t;

bool telemetry_ring_init(telemetry_ring_t *ring, telemetry_record_t *memory, size_t capacity);
void telemetry_ring_push(telemetry_ring_t *ring, const heater_telemetry_t *frame);
telemetry_stats_t telemetry_ring_stats(const telemetry_ring_t *ring);
// from=0 means the oldest retained record. next is the cursor after this batch.
size_t telemetry_ring_read(const telemetry_ring_t *ring, uint64_t from,
                           telemetry_record_t *output, size_t limit,
                           uint64_t *next, uint64_t *lost);
