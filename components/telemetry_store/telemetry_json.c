#include "telemetry_json.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

// 先将四路电阻分别编码为数值或 null，再按固定数组字段顺序拼接一条记录。
// 同时检查有效位和 isfinite，避免输出 JSON 不支持的 NAN/无穷大。
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
    const heater_experiment_t *e=&f->experiment;
    const int length = snprintf(output, capacity,
        "[%" PRIu64 ",%" PRIu32 ",%" PRIu64 ",[%.1f,%.1f,%.1f,%.1f],[%s,%s,%s,%s],%u,%" PRIu32
        ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ","
        "{\"run_id\":%u,\"config_id\":%u,\"mode\":%u,\"algorithm\":%u,"
        "\"target\":[%.6f,%.6f,%.6f,%.6f],\"requested\":[%.6f,%.6f,%.6f,%.6f],"
        "\"limited_mask\":%u,\"blocked_mask\":%u,\"source_sequence\":%u,\"computation_us\":%u}]",
        record->cursor, f->sequence, f->cycle_start_us,
        e->mode ? e->pwm_ticks[0]*0.5 : f->duty[0], e->mode ? e->pwm_ticks[1]*0.5 : f->duty[1],
        e->mode ? e->pwm_ticks[2]*0.5 : f->duty[2], e->mode ? e->pwm_ticks[3]*0.5 : f->duty[3],
        resistance[0], resistance[1], resistance[2], resistance[3], f->valid_mask,
        f->faults, f->deadline_misses, f->telemetry_drops, f->control_stack_free_bytes,
        (unsigned)e->run_id,(unsigned)e->config_id,e->mode,e->algorithm,
        (double)e->target[0],(double)e->target[1],(double)e->target[2],(double)e->target[3],
        (double)e->requested[0],(double)e->requested[1],(double)e->requested[2],(double)e->requested[3],
        e->limited_mask,e->blocked_mask,(unsigned)e->source_sequence,(unsigned)e->computation_us);
    return length < 0 || (size_t)length >= capacity ? -1 : length;
}

// 逐位解析十进制并在乘十前检查 uint64_t 溢出，只在完整成功后写回输出。
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

// 将 `c` 对应的十六进制数字转换为 0～15，其他字符返回 -1。
static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 按 & 和 = 精确匹配字段，逐字节解码值并拒绝重复字段、控制字符及截断。
// 空值允许成功返回；容量检查始终为结尾 NUL 预留一字节。
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
            if (found) return false; // 拒绝有歧义的重复字段
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
