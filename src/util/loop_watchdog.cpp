#include "util/loop_watchdog.h"

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C) && !defined(FLIGHTSCNR_SIM)

#include <Arduino.h>
#include <Update.h>
#include <WiFi.h>
#include <esp_task_wdt.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>

namespace util::loopwdt {
namespace {

/** loop() silent this long with Wi-Fi up = hung. Longest legit pass seen: a few s. */
constexpr uint32_t kHangMs = 45000;
/** Wi-Fi down: wifiReconnect() may block up to ~2.5 min (3 networks x 3 x 15 s). */
constexpr uint32_t kHangWifiDownMs = 300000;
/** Task-watchdog timeout: it bites this long after the monitor stops feeding it, so a
 *  hang with Wi-Fi up restarts the device after about 45 + 15 = 60 s. */
constexpr uint32_t kTwdtTimeoutMs = 15000;
constexpr uint32_t kCheckMs = 1000;
/** Above loop() (1) and the touch task (2) on core 1, so a spinning loop() cannot
 *  starve it. It runs a few microseconds a second. */
constexpr UBaseType_t kMonitorPriority = 5;
constexpr uint32_t kMonitorStackBytes = 3072;

std::atomic<uint32_t> s_last_alive_ms{0};
std::atomic<TaskHandle_t> s_loop_task{nullptr};
esp_task_wdt_user_handle_t s_user = nullptr;

void monitorTask(void*) {
  bool warned = false;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(kCheckMs));
    const uint32_t age = millis() - s_last_alive_ms.load();
    // WiFi.status() reads state kept from Wi-Fi events; it is not an RPC to the C6.
    const bool wifi_up = WiFi.status() == WL_CONNECTED;
    const uint32_t limit = wifi_up ? kHangMs : kHangWifiDownMs;
    if (age < limit) {
      esp_task_wdt_reset_user(s_user);
      warned = false;
      continue;
    }
    if (!warned) {
      warned = true;
      Serial.printf("[wdt] loop silent %lus (wifi %s) - restarting in %lus\n",
                    static_cast<unsigned long>(age / 1000U), wifi_up ? "up" : "down",
                    static_cast<unsigned long>(kTwdtTimeoutMs / 1000U));
    }
  }
}

}  // namespace

void start() {
  if (s_loop_task.load() != nullptr) {
    return;
  }
  // idle_core_mask 0: the idle tasks stay unwatched (heavy TLS on core 0 can starve
  // them for seconds by design). Only the "loop" user below is watched.
  const esp_task_wdt_config_t cfg = {
      .timeout_ms = kTwdtTimeoutMs,
      .idle_core_mask = 0,
      .trigger_panic = true,
  };
  esp_err_t err = esp_task_wdt_reconfigure(&cfg);
  if (err == ESP_OK) {
    err = esp_task_wdt_add_user("loop", &s_user);
  }
  if (err != ESP_OK) {
    Serial.printf("[wdt] loop watchdog not armed: %s\n", esp_err_to_name(err));
    return;
  }
  s_last_alive_ms.store(millis());
  s_loop_task.store(xTaskGetCurrentTaskHandle());
  if (xTaskCreatePinnedToCore(monitorTask, "loopwdt", kMonitorStackBytes, nullptr,
                              kMonitorPriority, nullptr, 1) != pdPASS) {
    esp_task_wdt_delete_user(s_user);
    s_user = nullptr;
    Serial.println("[wdt] loop watchdog not armed: task create failed");
    return;
  }
  // A browser firmware upload writes flash from inside one WebServer request on the
  // loop task; each flash write reports progress here.
  Update.onProgress([](size_t, size_t) { keepAlive(); });
  Serial.printf("[wdt] loop watchdog armed: %lus silent (Wi-Fi up) / %lus (Wi-Fi down) + %lus\n",
                static_cast<unsigned long>(kHangMs / 1000U),
                static_cast<unsigned long>(kHangWifiDownMs / 1000U),
                static_cast<unsigned long>(kTwdtTimeoutMs / 1000U));
}

void notePass() { s_last_alive_ms.store(millis(), std::memory_order_relaxed); }

void keepAlive() {
  const TaskHandle_t loop_task = s_loop_task.load(std::memory_order_relaxed);
  if (loop_task != nullptr && xTaskGetCurrentTaskHandle() == loop_task) {
    s_last_alive_ms.store(millis(), std::memory_order_relaxed);
  }
}

}  // namespace util::loopwdt

#else

namespace util::loopwdt {
void start() {}
void notePass() {}
void keepAlive() {}
}  // namespace util::loopwdt

#endif
