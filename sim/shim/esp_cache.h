#pragma once
#include <stddef.h>
#include "esp_sim_common.h"
#define ESP_CACHE_MSYNC_FLAG_INVALIDATE (1 << 0)
#define ESP_CACHE_MSYNC_FLAG_UNALIGNED (1 << 1)
#define ESP_CACHE_MSYNC_FLAG_DIR_C2M (1 << 2)
#define ESP_CACHE_MSYNC_FLAG_DIR_M2C (1 << 3)
// The sim window reads the framebuffer directly; nothing to write back.
static inline esp_err_t esp_cache_msync(void*, size_t, int) { return ESP_OK; }
