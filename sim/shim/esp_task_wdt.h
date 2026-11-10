#pragma once
#include "esp_sim_common.h"
static inline esp_err_t esp_task_wdt_reset(void) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_add(void*) { return ESP_OK; }
static inline esp_err_t esp_task_wdt_delete(void*) { return ESP_OK; }
