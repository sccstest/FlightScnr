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

/** Let the prior TLS session release buffers before opening another connection. */
inline void drainTlsHeapAfterSession(uint32_t timeout_ms = 800) {
  vTaskDelay(pdMS_TO_TICKS(150));
  const unsigned long started = millis();
  const unsigned long deadline = started + timeout_ms;
  while (static_cast<long>(millis() - deadline) < 0) {  // wrap-safe
    const uint32_t blk = ESP.getMaxAllocHeap();
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
