#pragma once

#include <cstddef>
#include <cstdint>

namespace services::ota_github {

/** Load NVS cache (latest tag / URL / available). Freshness is per-boot. */
void init();

/**
 * If Wi‑Fi is up and either this boot has not checked yet, or 24h of uptime
 * have passed since the last successful check, queue a GitHub releases/latest
 * check on the shared HTTPS worker. Never blocks. Safe to call every loop pass.
 */
void pollIfDue();

/**
 * Never blocks. With force, queues a GitHub release check on the shared HTTPS
 * worker (unless one is already queued). Returns true when a check succeeded this
 * boot and is still within the daily window (HTTP 404 "no release" counts as a
 * success); poll checkInProgress() / call again for the result.
 */
bool checkLatest(bool force);

/** True while a release check is queued or running. */
bool checkInProgress();

/**
 * After an OTA install the new image boots "pending verify"; this marks it good so
 * the bootloader keeps it (main's loop() calls it after 2 min of normal running; the
 * first good adsb.fi fetch and pollIfDue's 30 min mark still call it too). Safe to
 * call often and from any task; no-op for a serial-flashed image.
 */
void confirmRunningImage(const char* why);

/** True when cached latest tag is newer than the running firmware version. */
bool updateAvailable();

/** Cached latest release tag (e.g. "2026.7.30.1"), or empty. */
const char* latestTag();

/** Running firmware version string (same as config::kFirmwareVersion). */
const char* currentVersion();

enum class InstallState : uint8_t {
  Idle = 0,
  Running,
  Succeeded,
  Failed,
};

/** Start background install of the cached latest app.bin. False if busy/unavailable. */
bool startInstall();

InstallState installState();
/** 0–100 while Running; 100 on Succeeded. */
uint8_t installPercent();
/** Bytes written / expected total (0 if unknown) for progress UI. */
uint32_t installBytes();
uint32_t installTotal();
/** Short error text when Failed; otherwise empty. */
const char* installError();

}  // namespace services::ota_github
