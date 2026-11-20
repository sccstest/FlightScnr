#pragma once

// One-line internal-RAM report for the serial log.
//
// On the P4, ESP.getFreeHeap()/getMaxAllocHeap() can look healthy while the RAM the
// Wi-Fi link needs is nearly gone: the figure includes a 31 KB RTC RAM block that is
// not DMA-capable. This prints the DMA-capable numbers too. No-op elsewhere.

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C) && !defined(FLIGHTSCNR_SIM)

#include <Arduino.h>
#include <esp_heap_caps.h>

namespace fs_util {

inline void logInternalHeap(const char* tag) {
  const uint32_t caps_dma = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
  Serial.printf("[heap] %s internal free=%u largest=%u | dma free=%u largest=%u min=%u\n", tag,
                static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                static_cast<unsigned>(heap_caps_get_free_size(caps_dma)),
                static_cast<unsigned>(heap_caps_get_largest_free_block(caps_dma)),
                static_cast<unsigned>(heap_caps_get_minimum_free_size(caps_dma)));
}

}  // namespace fs_util

#else

namespace fs_util {
inline void logInternalHeap(const char*) {}
}  // namespace fs_util

#endif
