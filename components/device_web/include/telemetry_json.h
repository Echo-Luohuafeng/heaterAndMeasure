#pragma once
#include "telemetry_ring.h"

// 将非空的 `record` 编码为紧凑 JSON 数组，写入可写的 `output`。
// 字段顺序为游标、序号、时间、四路占空比、电阻、有效掩码、故障、
// 时序迟到数、队列丢帧数和控制任务栈余量（字节）。
// 无效或非有限电阻写为 null；`capacity` 含 NUL，返回不含 NUL 的长度。
// 失败或空间不足返回 -1，缓冲区可能包含截断文本。
int telemetry_record_json(const telemetry_record_t* record, char* output,
                          size_t capacity);
// 将 NUL 结尾的十进制 `text` 解析到非空的 `value`。
// 允许零；NULL、空串、非数字或 uint64_t 溢出返回 false 且不改写输出。
bool telemetry_parse_cursor(const char* text, uint64_t* value);
// 从 NUL 结尾的 `body` 提取唯一的 `key` 字段，并进行 %HH 和加号解码。
// `body`、`key`、`value` 均须非空，`capacity` 包含输出结尾 NUL。
// 字段缺失、重复、非法编码、控制字符或容量不足返回 false。
// 失败时输出可能已被部分改写，调用方仅在返回 true 时使用结果。
bool telemetry_form_value(const char* body, const char* key, char* value,
                          size_t capacity);
