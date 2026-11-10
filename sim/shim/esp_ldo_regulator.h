#pragma once
#include "esp_sim_common.h"
typedef struct {
  int chan_id;
  int voltage_mv;
} esp_ldo_channel_config_t;
typedef void* esp_ldo_channel_handle_t;
static inline esp_err_t esp_ldo_acquire_channel(const esp_ldo_channel_config_t*, esp_ldo_channel_handle_t* out) {
  *out = (void*)1;
  return ESP_OK;
}
