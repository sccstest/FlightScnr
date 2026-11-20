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
namespace {
bool s_sim_reconnecting = false;
}
bool wifiReconnectStart() {
  if (!s_sim_reconnecting) {
    Serial.println("WiFi reconnecting (sim, non-blocking)...");
  }
  s_sim_reconnecting = true;
  return true;
}
bool wifiReconnectPoll() {
  if (!s_sim_reconnecting || WiFi.status() != WL_CONNECTED) {
    return false;
  }
  s_sim_reconnecting = false;
  Serial.println("[wifi] reconnected (sim)");
  return true;
}
bool wifiReconnectActive() { return s_sim_reconnecting; }
void wifiNoteLinkUp() {}
void wifiRestartOutageClock() { Serial.println("[sim] Wi-Fi outage clock restarted"); }
bool wifiSoftRecycle() {
  Serial.println("[sim] Wi-Fi recycle (no-op)");
  return true;
}
bool wifiEscalationRestart(const char* why) {
  // The device would esp_restart() here; the simulator only logs it.
  Serial.printf("[net] restarting the device: %s (sim: not restarting)\n",
                why != nullptr ? why : "?");
  return false;
}
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

namespace {
bool s_sim_web_active = false;  // so main's stop/restart logic can be exercised
}
void settingsWebStart() {
  if (!s_sim_web_active) {
    Serial.println("[sim] settings web: started (stub)");
  }
  s_sim_web_active = true;
}
void settingsWebStop() {
  if (s_sim_web_active) {
    Serial.println("[sim] settings web: stopped (stub)");
  }
  s_sim_web_active = false;
}
void settingsWebPoll() {}
bool settingsWebActive() { return s_sim_web_active; }

namespace services::ota_github {
void init() {}
void pollIfDue() {}
bool checkLatest(bool) { return false; }
bool checkInProgress() { return false; }
void confirmRunningImage(const char*) {}
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
