#include "telemetry_ring.h"

// 同时检查运行开关、消息类型及运行 ID，过滤停止后或旧运行残留的队列数据。
bool telemetry_message_recordable(const heater_message_t *message, bool running, uint32_t run_id)
{
    return running && run_id != 0 && message->kind == HEATER_MESSAGE_DATA && message->run_id == run_id;
}

// 检查调用方提供的存储及容量后重置元数据，从游标 1 开始，不清空记录内存。
bool telemetry_ring_init(telemetry_ring_t *ring, telemetry_record_t *memory, size_t capacity)
{
    if (!ring || !memory || !capacity) return false;
    *ring = (telemetry_ring_t){.records = memory, .capacity = capacity, .next = 1};
    return true;
}

// 由 next 与容量推算最旧游标及覆盖总数，不额外维护计数器。
telemetry_stats_t telemetry_ring_stats(const telemetry_ring_t *ring)
{
    const uint64_t oldest = ring->next > ring->capacity ? ring->next - ring->capacity : 1;
    return (telemetry_stats_t){.oldest = oldest, .next = ring->next,
        .overwritten = oldest - 1, .capacity = ring->capacity};
}

// 以 (next - 1) 对容量取模定位写槽，复制帧和游标后推进 next。
void telemetry_ring_push(telemetry_ring_t *ring, const heater_telemetry_t *frame)
{
    ring->records[(ring->next - 1) % ring->capacity] =
        (telemetry_record_t){.cursor = ring->next, .frame = *frame};
    ++ring->next;
}

// 将请求游标裁剪到保留范围并计算覆盖丢失数，再按槽位顺序复制批次。
// 返回条数及后续游标；不自行加锁，依赖上层保证读取期间无并发覆盖。
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
