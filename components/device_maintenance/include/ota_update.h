#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// 定义 OTA 数据读取回调，context 原样传递，buffer 须可写且可容纳 size 字节。
// 返回实际读取的 1～size 字节；零或负值表示结束/失败，接收流程会中止。
typedef int (*ota_read_fn)(void* context, void* buffer, size_t size);
// 定义 OTA 写入进度回调，received 为累计成功写入的镜像字节数。
// 在 OTA 工作任务中同步调用，context 与读取回调一致。
typedef void (*ota_progress_fn)(void* context, size_t received);

// 通过非空的 `read` 回调接收 `total` 字节应用镜像，校验后选择下一启动分区。
// `context` 原样传给回调，可为 NULL；`progress` 可为
// NULL，否则报告累计写入字节数。 `require_sampling`
// 决定新版本是否必须通过采样自检，保存在 NVS 中。 写 Flash
// 前等待控制暂停确认；失败后保持停热，只有完整校验通过才切换启动分区。 返回
// ESP_OK 或参数、暂停、内存、读取、NVS、镜像校验及 Flash 操作错误。
esp_err_t ota_update_receive(size_t total, ota_read_fn read,
                             ota_progress_fn progress, void* context,
                             bool require_sampling);
// 登记首次 USB 镜像的有效基线，或根据 `healthy` 确认待验证镜像。
// 要求采样自检但不健康时触发回滚重启；`controller_ready` 表示控制初始化成功。
// 元数据写入前等待暂停确认；确认成功且控制可用时恢复空闲安全采样。
// 返回 ESP_OK 或状态、暂停、NVS/OTA 操作错误；回滚成功通常不返回。
esp_err_t ota_update_confirm(bool healthy, bool controller_ready);
