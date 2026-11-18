#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "telemetry_ring.h"

// 初始化带互斥保护的历史缓存，优先分配 4 MiB PSRAM。
// 不可用时降级为 32 KiB 内部 RAM；历史仅保存在易失内存中。
// 返回 ESP_OK 或重复初始化、内存分配错误。
esp_err_t telemetry_store_init(void);
// 从非空的 `samples` 队列接收 heater_message_t，启动 Core 0 缓存任务。
// 要求缓存已初始化；队列须保持有效，旧运行及停热后的样本不会写入历史。
// 返回 ESP_OK 或状态、任务创建错误。
esp_err_t telemetry_store_start(QueueHandle_t samples);
// 在控制 STOP 后等待已接受的缓存写入结束，再允许发送停止确认。
// 要求缓存已初始化；通过互斥锁形成屏障，只能在任务中阻塞调用。
void telemetry_store_sync_stop(void);
// 返回历史缓存是否成功使用 PSRAM；初始化后结果保持不变。
bool telemetry_store_uses_psram(void);
// 加锁读取历史缓存的游标、容量和覆盖计数，返回一致快照。
// 要求缓存已初始化，只能在任务中调用。
telemetry_stats_t telemetry_store_stats(void);
// 将最近的历史记录复制到非空的 `frame`，不包含空闲安全样本。
// 缓存有记录时返回 true；否则返回 false 且不改写输出。
// 要求缓存已初始化，只能在任务中调用。
bool telemetry_store_latest(heater_telemetry_t* frame);
// 加锁从 `from` 读取至多 `limit` 条历史记录到 `output`，返回实际条数。
// `from=0` 从最旧记录开始；output 须可容纳 limit 条记录。
// 非空的 `next`、`lost`、`stats` 接收后续游标、覆盖丢失数和同次锁内的统计快照。
// 要求缓存已初始化，只能在任务中调用。
size_t telemetry_store_read(uint64_t from, telemetry_record_t* output,
                            size_t limit, uint64_t* next, uint64_t* lost,
                            telemetry_stats_t* stats);

// 接收门由实时内核通过注入回调更新，STOP 的旧代次消息不再写入历史。
void telemetry_store_set_run(bool running, uint32_t run_id,
                             uint32_t generation);
// 写入不可变配置并固定当前配置；历史引用未释放时容量不足会拒绝新实验。
esp_err_t telemetry_store_prepare_config(const heater_config_record_t* record);
// 在同一缓存锁内复制配置表，调用方提供容量；序列化在锁外进行。
unsigned telemetry_store_configurations(heater_config_record_t* records,
                                        unsigned capacity);
