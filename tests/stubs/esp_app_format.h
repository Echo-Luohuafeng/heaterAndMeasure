// 原生测试替身，用于验证生产组件的接口与时序。
#pragma once
#include <stdint.h>
#define ESP_IMAGE_HEADER_MAGIC 0xE9
#define ESP_CHIP_ID_ESP32S3 9
typedef struct __attribute__((packed)) {
  uint8_t magic;
  uint8_t before_chip[11];
  uint16_t chip_id;
  uint8_t remaining[10];
} esp_image_header_t;
typedef struct {
  uint32_t load_addr, data_len;
} esp_image_segment_header_t;
_Static_assert(sizeof(esp_image_header_t) == 24, "IDF image header layout");
