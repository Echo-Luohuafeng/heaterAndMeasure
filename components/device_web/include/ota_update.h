#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

typedef int (*ota_read_fn)(void *context, void *buffer, size_t size);
typedef void (*ota_progress_fn)(void *context, size_t received);

// Pauses the real controller before any flash operation. Failed/truncated images
// leave output inhibited; only a fully validated image reaches boot selection.
esp_err_t ota_update_receive(size_t total, ota_read_fn read, ota_progress_fn progress,
                             void *context, bool require_sampling);
// Registers the first USB image as a rollback baseline, or confirms/rolls back
// a pending image. Metadata writes also require an acknowledged control pause.
esp_err_t ota_update_confirm(bool healthy, bool controller_ready);
