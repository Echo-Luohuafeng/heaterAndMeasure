// 原生测试替身，用于验证生产组件的接口与时序。
#pragma once
#include <stdint.h>
#define ESP_APP_DESC_MAGIC_WORD 0xABCD5432
typedef struct {
  uint32_t magic_word;
  uint8_t preceding[44];
  char project_name[32];
  uint8_t remaining[176];
} esp_app_desc_t;
_Static_assert(sizeof(esp_app_desc_t) == 256,
               "IDF application description layout");
const esp_app_desc_t* esp_app_get_description(void);
