// 原生测试显式模拟无 PSRAM，验证内部 RAM 降级路径。
#pragma once
#include <stdbool.h>
bool esp_psram_is_initialized(void);
