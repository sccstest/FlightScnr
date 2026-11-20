// Update the ESP32-C6's ESP-Hosted firmware from the P4, with the image embedded in
// this sketch. Uses the same calls as Arduino's ESP_HostedOTA library, minus the
// download, so it works when Wi-Fi itself is the thing that is broken.
//
// Safety:
//  - Nothing is written until the line "UPDATE-C6" arrives on the USB serial port.
//  - The image goes to the C6's spare OTA slot; the running firmware is only replaced
//    when the transfer has finished and the C6 has checked the image.
//  - If the transfer fails part-way, the sketch stops and the C6 keeps its firmware.
//
// Every boot prints "[c6] host=a.b.c c6=x.y.z". c6=0.0.0 means the C6 firmware is too
// old to answer the version query (older than 2.12.2).

#include <Arduino.h>
#include <WiFi.h>
#include "esp32-hal-hosted.h"

extern "C" const uint8_t c6fw_start[];
extern "C" const uint8_t c6fw_end[];

namespace {

constexpr size_t kChunk = 2048;  // same as ESP_HostedOTA
char s_line[32];
size_t s_line_len = 0;
bool s_done = false;

void printVersions(const char *when) {
  uint32_t hM = 0, hm = 0, hp = 0, sM = 0, sm = 0, sp = 0;
  hostedHasUpdate();  // refreshes the cached C6 version (times out on old firmware)
  hostedGetHostVersion(&hM, &hm, &hp);
  hostedGetSlaveVersion(&sM, &sm, &sp);
  Serial.printf("[c6] %s: host=%lu.%lu.%lu c6=%lu.%lu.%lu\n", when, hM, hm, hp, sM, sm, sp);
}

bool runUpdate() {
  const size_t total = static_cast<size_t>(c6fw_end - c6fw_start);
  Serial.printf("[c6] update: image %u bytes\n", static_cast<unsigned>(total));
  if (total < 100000 || c6fw_start[0] != 0xE9) {
    Serial.println("[c6] update: embedded image looks wrong, not starting");
    return false;
  }
  if (!hostedBeginUpdate()) {
    Serial.println("[c6] update: BEGIN failed (C6 kept its firmware)");
    return false;
  }
  static uint8_t buf[kChunk];
  size_t sent = 0;
  int last_pct = -1;
  while (sent < total) {
    const size_t n = (total - sent) < kChunk ? (total - sent) : kChunk;
    memcpy(buf, c6fw_start + sent, n);
    if (!hostedWriteUpdate(buf, n)) {
      Serial.printf("[c6] update: WRITE failed at %u of %u (C6 kept its firmware)\n",
                    static_cast<unsigned>(sent), static_cast<unsigned>(total));
      return false;
    }
    sent += n;
    const int pct = static_cast<int>(sent * 100 / total);
    if (pct / 10 != last_pct / 10) {
      Serial.printf("[c6] update: %d%%\n", pct);
      last_pct = pct;
    }
    delay(2);
  }
  if (!hostedEndUpdate()) {
    Serial.println("[c6] update: END failed (C6 kept its firmware)");
    return false;
  }
  Serial.println("[c6] update: image accepted, activating");
  if (!hostedActivateUpdate()) {
    Serial.println("[c6] update: activate reported an error (normal on old firmware)");
  }
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(4000);  // let the PC reopen the USB port
  Serial.println("[c6] updater start");
  WiFi.mode(WIFI_STA);  // brings up ESP-Hosted
  printVersions("boot");
  Serial.println("[c6] waiting for the line UPDATE-C6");
}

void loop() {
  while (Serial.available() > 0 && !s_done) {
    const char c = static_cast<char>(Serial.read());
    if (c == '\n' || c == '\r') {
      s_line[s_line_len] = '\0';
      if (strcmp(s_line, "UPDATE-C6") == 0) {
        s_done = true;
        const bool ok = runUpdate();
        Serial.printf("[c6] update result: %s\n", ok ? "OK" : "FAILED");
        if (ok) {
          Serial.println("[c6] restarting the P4 in 8 s to re-link with the C6");
          delay(8000);
          ESP.restart();
        }
      }
      s_line_len = 0;
    } else if (s_line_len + 1 < sizeof(s_line)) {
      s_line[s_line_len++] = c;
    }
  }
  static uint32_t last = 0;
  if (!s_done && millis() - last > 10000) {
    last = millis();
    Serial.println("[c6] idle, waiting for UPDATE-C6");
  }
  delay(10);
}
