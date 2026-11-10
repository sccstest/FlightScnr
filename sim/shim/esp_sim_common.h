#pragma once
// Attribute / logging macros the ESP32 toolchain normally provides.

#include <stdint.h>

#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_ATTR
#define RTC_DATA_ATTR
#define RTC_NOINIT_ATTR
#define WORD_ALIGNED_ATTR __attribute__((aligned(4)))
#define ARDUINO_ISR_ATTR

#define log_e(...) ((void)0)
#define log_w(...) ((void)0)
#define log_i(...) ((void)0)
#define log_d(...) ((void)0)
#define log_v(...) ((void)0)
#define ESP_LOGE(tag, ...) ((void)0)
#define ESP_LOGW(tag, ...) ((void)0)
#define ESP_LOGI(tag, ...) ((void)0)
#define ESP_LOGD(tag, ...) ((void)0)
#define ESP_LOGV(tag, ...) ((void)0)

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_TIMEOUT 0x107

#ifdef __cplusplus
extern "C" {
#endif
const char* esp_err_to_name(esp_err_t code);
#ifdef __cplusplus
}
#endif
