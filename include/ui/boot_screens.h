#pragma once

/** Full-screen messages during WiFi setup and reconnect. */
void bootScreenShowPortalHint();
void bootScreenShowConnectFailed();
void bootScreenShowWifiCleared();
/** "Connected" + network name + the settings page address by IP (mDNS often fails). */
void bootScreenShowConnected(const char* ssid, const char* ip);

void bootScreenConnectingStart(const char* ssid);
void bootScreenConnectingPulse();

/**
 * Knob-hold Wi-Fi wipe countdown: rim ring + "Resetting Wi-Fi in N".
 * Call while the knob is held; held_ms is time since press, total_ms is the wipe hold.
 */
void bootScreenWifiResetCountdownTick(unsigned long held_ms, unsigned long total_ms);
/** Clear countdown UI state (call when the knob is released early). */
void bootScreenWifiResetCountdownCancel();
/** True while the countdown UI is on screen. */
bool bootScreenWifiResetCountdownActive();
