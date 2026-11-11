// Stand-ins for modules the simulator leaves out:
//  - wifi_setup.cpp   (WiFiManager captive portal) -> the PC is always online
//  - settings_web.cpp (on-device web server)       -> set things with sim flags / sim_data/nvs.txt
//  - ota_github.cpp   (firmware self-update)       -> nothing to flash
#include <Arduino.h>
#include <WiFi.h>

#include "services/ota_github.h"
#include "services/settings_web.h"
#include "services/wifi_setup.h"

bool wifiShowsSetupScreenOnBoot() { return false; }
void wifiResetCredentialsAndReboot() {
  Serial.println("[sim] Wi-Fi reset requested (no-op in the simulator)");
}
bool wifiSetupConnect() {
  Serial.println("[sim] Wi-Fi: using the PC's network");
  return true;
}
// With --offline / --wifi-down the shim's WiFi.status() reports disconnected;
// "reconnecting" succeeds once that window ends.
bool wifiReconnect() { return WiFi.status() == WL_CONNECTED; }
bool wifiSoftRecycle() { return true; }
uint8_t wifiNetsCount() { return 1; }
bool wifiNetsGetSsid(uint8_t index, char* out, size_t out_len) {
  if (index != 0 || out == nullptr || out_len == 0) return false;
  snprintf(out, out_len, "%s", "Simulator (PC network)");
  return true;
}
bool wifiNetsIsDemoted(uint8_t) { return false; }
bool wifiNetsRemove(uint8_t) { return false; }
bool wifiNetsSwap(uint8_t, uint8_t) { return false; }
bool wifiNetsMoveUp(uint8_t) { return false; }
bool wifiNetsMoveDown(uint8_t) { return false; }

void settingsWebStart() {}
void settingsWebStop() {}
void settingsWebPoll() {}
bool settingsWebActive() { return false; }

namespace services::ota_github {
void init() {}
void pollIfDue() {}
bool checkLatest(bool) { return false; }
bool updateAvailable() { return false; }
const char* latestTag() { return ""; }
const char* currentVersion() { return "sim"; }
bool startInstall() { return false; }
InstallState installState() { return InstallState{}; }
uint8_t installPercent() { return 0; }
uint32_t installBytes() { return 0; }
uint32_t installTotal() { return 0; }
const char* installError() { return ""; }
}  // namespace services::ota_github
