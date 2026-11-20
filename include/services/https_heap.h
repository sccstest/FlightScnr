#pragma once

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
namespace services::https {

/** ADS-B poll (~2s JSON) — lower bar than route APIs. */
inline bool heapReadyForAdsb() {
  return ESP.getFreeHeap() >= config::kMinFreeHeapForAdsbHttps &&
         ESP.getMaxAllocHeap() >= config::kMinContiguousHeapForAdsbTls;
}

/** FlightAware / AirLabs / FR24 detail enrichment. */
inline bool heapReadyForRouteApi() {
  return ESP.getFreeHeap() >= config::kMinFreeHeapForRouteHttps &&
         ESP.getMaxAllocHeap() >= config::kMinContiguousHeapForRouteTls;
}

/** Planespotters photo fetch (meta + JPEG). Slightly looser than route APIs. */
inline bool heapReadyForPhoto() {
  return ESP.getFreeHeap() >= config::kMinFreeHeapForPhotoHttps &&
         ESP.getMaxAllocHeap() >= config::kMinContiguousHeapForPhotoTls;
}

/** Largest free block the TLS drain judges. On the 4C getMaxAllocHeap() includes a
 *  31 KB RTC block the Wi-Fi link and lwIP cannot use (so the drain never waited):
 *  judge internal DMA-capable RAM there, like main's heap ladder, and count it only
 *  while its free total also clears the ADS-B bar. */
inline uint32_t drainHeapBlock() {
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  constexpr uint32_t caps = MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL;
  if (heap_caps_get_free_size(caps) < config::kMinFreeHeapForAdsbHttps) {
    return 0;
  }
  return static_cast<uint32_t>(heap_caps_get_largest_free_block(caps));
#else
  return ESP.getMaxAllocHeap();
#endif
}

/** Let the prior TLS session release buffers before opening another connection.
 *  Worker tasks only on the 4C (it sleeps). */
inline void drainTlsHeapAfterSession(uint32_t timeout_ms = 800) {
  vTaskDelay(pdMS_TO_TICKS(150));
  const unsigned long started = millis();
  const unsigned long deadline = started + timeout_ms;
  while (static_cast<long>(millis() - deadline) < 0) {  // wrap-safe
    const uint32_t blk = drainHeapBlock();
    if (blk >= config::kMinContiguousHeapForTlsReconnect) {
      return;
    }
    if (blk >= config::kMinContiguousHeapForAdsbTls && millis() - started >= 350) {
      return;
    }
    vTaskDelay(pdMS_TO_TICKS(40));
  }
}

/** Largest free block of internal DMA-capable RAM: what ESP-Hosted, lwIP and the
 *  TLS handshake actually need. getMaxAllocHeap() also counts RAM they can't use. */
inline uint32_t dmaLargestFreeBlock() {
  return static_cast<uint32_t>(
      heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
}

/** True when internal DMA-capable RAM is so short that a Wi-Fi recycle may help. */
inline bool tlsDmaRamLow() { return dmaLargestFreeBlock() < config::kTlsRecycleDmaBlockBytes; }

/** @deprecated Use heapReadyForAdsb or heapReadyForRouteApi. */
inline bool heapReady() { return heapReadyForRouteApi(); }

}  // namespace services::https
