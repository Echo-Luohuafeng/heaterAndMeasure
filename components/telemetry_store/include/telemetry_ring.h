#pragma once

#include "heater_domain.h"

// 保存带历史游标的周期帧，cursor 从 1 递增，独立于控制周期序号。
typedef struct {
  uint64_t cursor;
  heater_telemetry_t frame;
} telemetry_record_t;

// 管理调用方提供的环形存储，不拥有 records 内存；capacity 单位为记录条数。
// next 为下一条待分配游标，0 保留为读取最旧数据的请求值。
// 本类型不含锁，多任务读写由 telemetry_store 或调用方统一同步。
typedef struct {
  telemetry_record_t* records;
  size_t capacity;
  uint64_t next;
} telemetry_ring_t;

// 保存历史缓存统计；有效游标区间为 [oldest, next)，capacity 为记录条数。
// overwritten 为本次初始化以来被覆盖的累计记录数，不表示队列丢帧。
typedef struct {
  uint64_t oldest;
  uint64_t next;
  uint64_t overwritten;
  size_t capacity;
} telemetry_stats_t;

// 用非空的 `memory` 和非零记录数 `capacity` 初始化非空的 `ring`。
// 不分配内存也不取得所有权，调用方须保持存储有效并自行同步并发访问。
// 成功返回 true 并从游标 1 开始；参数非法返回 false，不改写 `ring`。
bool telemetry_ring_init(telemetry_ring_t* ring, telemetry_record_t* memory,
                         size_t capacity);
// 复制非空的 `frame` 到已初始化的非空 `ring`，分配递增游标。
// 容量满时覆盖最旧记录；调用方须同步并发读写。
void telemetry_ring_push(telemetry_ring_t* ring,
                         const heater_telemetry_t* frame);
// 返回已初始化的非空 `ring` 的保留游标范围、覆盖总数和记录容量。
// 调用方须同步并发读写，返回的 next 为下一条待分配游标。
telemetry_stats_t telemetry_ring_stats(const telemetry_ring_t* ring);
// 从 `from` 游标复制至多 `limit` 条记录到 `output`，返回实际条数。
// `from=0` 从最旧记录开始；过旧游标自动前移，过新游标截到缓存末尾。
// 非空的 `next`、`lost` 分别接收批次后游标及因覆盖丢失的记录数。
// `ring` 须已初始化且非空，`output` 须能容纳 limit 条；调用方负责并发同步。
size_t telemetry_ring_read(const telemetry_ring_t* ring, uint64_t from,
                           telemetry_record_t* output, size_t limit,
                           uint64_t* next, uint64_t* lost);
