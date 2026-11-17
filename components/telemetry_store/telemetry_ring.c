#include "telemetry_ring.h"

bool telemetry_ring_init(telemetry_ring_t *ring, telemetry_record_t *memory, size_t capacity)
{
    if (!ring || !memory || !capacity) return false;
    *ring = (telemetry_ring_t){.records = memory, .capacity = capacity, .next = 1};
    return true;
}

telemetry_stats_t telemetry_ring_stats(const telemetry_ring_t *ring)
{
    const uint64_t oldest = ring->next > ring->capacity ? ring->next - ring->capacity : 1;
    return (telemetry_stats_t){.oldest = oldest, .next = ring->next,
        .overwritten = oldest - 1, .capacity = ring->capacity};
}

void telemetry_ring_push(telemetry_ring_t *ring, const heater_telemetry_t *frame)
{
    ring->records[(ring->next - 1) % ring->capacity] =
        (telemetry_record_t){.cursor = ring->next, .frame = *frame};
    ++ring->next;
}

size_t telemetry_ring_read(const telemetry_ring_t *ring, uint64_t from,
                           telemetry_record_t *output, size_t limit,
                           uint64_t *next, uint64_t *lost)
{
    const telemetry_stats_t stats = telemetry_ring_stats(ring);
    *lost = from && from < stats.oldest ? stats.oldest - from : 0;
    if (!from || from < stats.oldest) from = stats.oldest;
    if (from > stats.next) from = stats.next;
    size_t count = 0;
    while (count < limit && from < stats.next) {
        output[count++] = ring->records[(from - 1) % ring->capacity];
        ++from;
    }
    *next = from;
    return count;
}
