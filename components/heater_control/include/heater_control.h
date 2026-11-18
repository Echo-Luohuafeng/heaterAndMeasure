#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "heater_protocol.h"
#include "heater_controller.h"

// 初始化输出、ADC、阶段定时器和 DRDY 中断，不启动周期任务。
// `outbound` 必须是可容纳 heater_message_t 的非空队列，并保持到任务结束。
// 应在 Core 1 调用以在同核分配中断；返回 ESP_OK 或初始化错误。
esp_err_t heater_control_init(QueueHandle_t outbound);
// 创建 Core 1 控制任务并启动 20 ms 周期的后台安全采样。
// 要求初始化成功且尚未启动；默认空闲，等待 START 才加热及记录。
// 返回 ESP_OK 或状态、任务创建、定时器错误。
esp_err_t heater_control_start(void);
// 提交非空的 `command`，在调用期间复制设定，不保留其指针。
// START 开始新运行；SET 仅更新设定；运行中的 SET/PING 续约。
// STOP 立即停热并取消记录；租约到期也取消运行，设定于周期边界生效。
// 维护期间只接受 STOP；返回 ESP_OK 或状态、参数、定时器错误。
esp_err_t heater_control_submit(const heater_command_t *command);
// 保存一致的运行快照；running 为显式运行状态，id 为最近分配的运行 ID。
// 启动前 id 为零，后续 START 分配非零 ID；停止后保留 ID 供识别旧消息。
typedef struct { bool running; uint32_t id; } heater_run_state_t;
// 返回受临界区保护的运行状态快照，用运行 ID 排除旧队列样本。
heater_run_state_t heater_control_run_state(void);
// 将最近的内部安全采样复制到非空的 `frame`，包括空闲时的样本。
// 有样本返回 true；否则返回 false 且不改写 `frame`，不依赖历史缓存。
bool heater_control_latest(heater_telemetry_t *frame);
// 进入维护状态并等待控制任务确认定时器和 ADC 操作均已停止。
// `timeout` 为获取生命周期锁及等待确认各自的 tick 上限，只能在任务中调用。
// 返回 ESP_OK 后才允许写 Flash；超时返回 ESP_ERR_TIMEOUT。
// 发起维护后等待确认超时仍保持输出禁止；获取锁超时则尚未发起本次暂停。
// 未初始化或无法确认安全关断时返回 ESP_ERR_INVALID_STATE。
esp_err_t heater_control_pause(TickType_t timeout);
// 从已确认暂停状态恢复后台安全采样，保持空闲及零设定。
// 只能在任务中调用；补偿暂停时间以保留采样时间轴上的缺口。
// 返回 ESP_OK 或状态、锁超时、定时器错误；加热和记录仍需显式 START。
esp_err_t heater_control_resume(void);
// 返回是否已确认停止周期定时器和 ADC 操作。
// 写 Flash 前应取得 heater_control_pause() 的成功确认。
bool heater_control_is_paused(void);
// 返回是否处于禁止常规控制命令的维护状态，可能仍在等待暂停确认。
bool heater_control_is_maintenance(void);
// 保存 ADC 时序诊断：最晚 START 完成/检查偏移（µs）、启动与读取截止失败次数。
typedef struct {
    uint32_t adc_start_max_offset_us;
    uint32_t adc_start_misses;
    uint32_t adc_read_misses;
} heater_timing_stats_t;
// 返回受临界区保护的 ADC 启动及读取时序统计快照。
heater_timing_stats_t heater_control_timing_stats(void);

// 仅空闲且非维护状态允许配置；不写 Flash、不开始运行。
esp_err_t heater_control_configure(const controller_config_t *config);
typedef struct {
    bool resistance_mode;
    bool ready;
    uint32_t config_id;
    controller_config_t config;
    uint32_t worker_stack_free_bytes;
    uint32_t controller_faults;
    uint32_t lease_refreshes, lease_remaining_ms;
} heater_controller_status_t;
heater_controller_status_t heater_control_controller_status(void);

typedef struct { uint32_t config_id; controller_config_t config; } heater_config_record_t;
// 最近八份恒电阻配置，运行期间不写 Flash；导出实验时应同时保存这些元数据。
unsigned heater_control_config_history(heater_config_record_t records[8]);
