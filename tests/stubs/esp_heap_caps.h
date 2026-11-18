// 原生测试替身，用于验证生产组件的接口与时序。
#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_SPIRAM 4
void* heap_caps_malloc(size_t, unsigned);
