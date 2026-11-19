// Bare Wi-Fi bring-up probe for the P4 + C6 (ESP-Hosted over SDIO). See platformio.ini.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include "esp32-hal-hosted.h"

static void report(const char *when) {
  uint32_t hM = 0, hm = 0, hp = 0, sM = 0, sm = 0, sp = 0;
  hostedGetHostVersion(&hM, &hm, &hp);
  hostedGetSlaveVersion(&sM, &sm, &sp);
  Serial.printf("[probe] %s: hosted_init=%d wifi_active=%d host=%lu.%lu.%lu c6=%lu.%lu.%lu "
                "internal_free=%u largest=%u\n",
                when, hostedIsInitialized(), hostedIsWiFiActive(), hM, hm, hp, sM, sm, sp,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  delay(4000);  // let the PC reopen the USB port
  Serial.println("[probe] start");
  report("before wifi");
  bool ok = WiFi.mode(WIFI_STA);
  Serial.printf("[probe] WiFi.mode(STA) -> %d\n", ok);
  report("after mode");
  Serial.printf("[probe] hostedHasUpdate=%d url=%s\n", hostedHasUpdate(), hostedGetUpdateURL());
  report("after version query");
  Serial.printf("[probe] MAC %s\n", WiFi.macAddress().c_str());
}

void loop() {
  int n = WiFi.scanNetworks();
  Serial.printf("[probe] scan -> %d networks\n", n);
  for (int i = 0; i < n && i < 8; ++i) {
    Serial.printf("[probe]   %d: ch%d %ddBm\n", i, WiFi.channel(i), WiFi.RSSI(i));
  }
  report("loop");
  delay(8000);
}
