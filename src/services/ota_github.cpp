#include "services/ota_github.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "services/secure_client.h"

#include <ArduinoJson.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "services/adsb_client.h"
#include "services/firmware_image.h"
#include "services/https_heap.h"
#include "services/https_lock.h"

namespace services::ota_github {

namespace {

constexpr char kStoreNs[] = "fs_ota_gh";
constexpr char kTagKey[] = "tag";
constexpr char kUrlKey[] = "url";
constexpr char kAvailKey[] = "av";

constexpr char kApiHost[] = "api.github.com";
// Boards built from a fork set FLIGHTSCNR_OTA_REPO in their board header so OTA
// pulls that fork's releases instead of upstream's.
#ifndef FLIGHTSCNR_OTA_REPO
#define FLIGHTSCNR_OTA_REPO "yashmulgaonkar/FlightScnr"
#endif
constexpr char kApiPath[] = "/repos/" FLIGHTSCNR_OTA_REPO "/releases/latest";
constexpr char kAppAssetName[] = "FlightScnr-" FLIGHTSCNR_BOARD_NAME "-app.bin";
constexpr char kUserAgent[] = "FlightScnr-OTA";

constexpr unsigned long kCheckIntervalMs = 24UL * 60UL * 60UL * 1000UL;
/** After a failed GitHub check (e.g. HTTP 403), wait before retrying. */
constexpr unsigned long kCheckRetryOnFailMs = 60UL * 60UL * 1000UL;
constexpr uint32_t kCheckTimeoutMs = 12000;
/** How long the background check waits for the shared HTTPS lock. */
constexpr uint32_t kCheckLockWaitMs = 15000;
/** No release check in the first minutes after boot: planes, weather and the time
 *  zone go first. */
constexpr unsigned long kFirstCheckAfterBootMs = 2UL * 60UL * 1000UL;
/** A new OTA image is also kept (marked valid) after this much uptime without a
 *  crash, so an internet outage right after an update can't roll it back. */
constexpr unsigned long kConfirmImageAfterMs = 30UL * 60UL * 1000UL;
constexpr uint32_t kInstallTimeoutMs = 10UL * 60UL * 1000UL;
constexpr size_t kStreamChunk = 4096;
constexpr size_t kOtaBreatherBytes = 16u * 1024u;

char s_latest_tag[32] = {};
char s_asset_url[256] = {};
bool s_available = false;
unsigned long s_last_check_ms = 0;
unsigned long s_check_cooldown_ms = kCheckIntervalMs;

volatile InstallState s_install_state = InstallState::Idle;
volatile uint8_t s_install_pct = 0;
volatile uint32_t s_install_bytes = 0;
volatile uint32_t s_install_total = 0;
char s_install_error[96] = {};
TaskHandle_t s_install_task = nullptr;

// Background release check (runs on the shared HTTPS worker).
volatile bool s_check_queued = false;  // queued or running
volatile bool s_check_last_ok = false;

const char* skipVersionPrefix(const char* s) {
  if (s == nullptr) {
    return "";
  }
  if ((s[0] == 'v' || s[0] == 'V') && s[1] != '\0') {
    return s + 1;
  }
  return s;
}

/** True when latest is strictly newer than current (dotted numeric). "dev" is always older. */
bool isNewerThanRunning(const char* latest_tag) {
  const char* latest = skipVersionPrefix(latest_tag);
  const char* current = skipVersionPrefix(config::kFirmwareVersion);
  if (latest[0] == '\0') {
    return false;
  }
  if (strcmp(current, "dev") == 0) {
    return true;
  }
  if (strcmp(latest, current) == 0) {
    return false;
  }

  const char* a = latest;
  const char* b = current;
  while (true) {
    char* a_end = nullptr;
    char* b_end = nullptr;
    const long av = strtol(a, &a_end, 10);
    const long bv = strtol(b, &b_end, 10);
    if (a_end == a && b_end == b) {
      return strcmp(a, b) > 0;
    }
    if (a_end == a) {
      return false;
    }
    if (b_end == b) {
      return true;
    }
    if (av != bv) {
      return av > bv;
    }
    a = (*a_end == '.') ? a_end + 1 : a_end;
    b = (*b_end == '.') ? b_end + 1 : b_end;
    if (*a == '\0' && *b == '\0') {
      return false;
    }
    if (*a == '\0') {
      return false;
    }
    if (*b == '\0') {
      return true;
    }
  }
}

void persistCache() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, false)) {
    return;
  }
  prefs.putString(kTagKey, s_latest_tag);
  prefs.putString(kUrlKey, s_asset_url);
  prefs.putBool(kAvailKey, s_available);
  prefs.end();
}

void loadCache() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    return;
  }
  String tag = prefs.getString(kTagKey, "");
  String url = prefs.getString(kUrlKey, "");
  s_available = prefs.getBool(kAvailKey, false);
  prefs.end();

  strncpy(s_latest_tag, tag.c_str(), sizeof(s_latest_tag) - 1);
  s_latest_tag[sizeof(s_latest_tag) - 1] = '\0';
  strncpy(s_asset_url, url.c_str(), sizeof(s_asset_url) - 1);
  s_asset_url[sizeof(s_asset_url) - 1] = '\0';
  // Recompute availability from tags; do not restore millis-based freshness —
  // that incorrectly skipped checks after reboot.
  if (s_latest_tag[0] != '\0') {
    s_available = isNewerThanRunning(s_latest_tag);
  }
}

/** True when a check already ran this boot and is still within the cooldown. */
bool cacheFresh() {
  if (s_last_check_ms == 0) {
    return false;
  }
  return (millis() - s_last_check_ms) < s_check_cooldown_ms;  // wrap-safe
}

void markCheckAttempt(bool success) {
  s_last_check_ms = millis() != 0 ? millis() : 1;
  s_check_cooldown_ms = success ? kCheckIntervalMs : kCheckRetryOnFailMs;
  s_check_last_ok = success;
}

size_t otaAppPartitionSize() {
  const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
  return next != nullptr ? static_cast<size_t>(next->size) : 0;
}

/** Runs on the shared HTTPS worker (core 0) via queueBackgroundJob, never on the
 *  loop: a TLS session to api.github.com takes seconds and used to freeze touch. */
bool fetchLatestFromGitHub() {
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  if (!services::https::heapReadyForRouteApi()) {
    Serial.println("[ota_gh] skip: low heap (retry later)");
    markCheckAttempt(false);
    return false;
  }

  // Waiting here only delays the worker's next job, not the UI.
  services::https::ScopedLock tls(kCheckLockWaitMs);
  if (!tls.held()) {
    Serial.println("[ota_gh] skip: could not take https lock");
    markCheckAttempt(false);
    return false;
  }
  services::https::drainTlsHeapAfterSession(400);

  services::https::SecureClient client;
  client.setInsecure();
  client.setTimeout(kCheckTimeoutMs / 1000);
  client.setHandshakeTimeout(kCheckTimeoutMs / 1000);

  HTTPClient http;
  http.setConnectTimeout(kCheckTimeoutMs);
  http.setTimeout(kCheckTimeoutMs);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  http.setReuse(false);
  http.addHeader("Accept", "application/vnd.github+json");

  char url[128];
  snprintf(url, sizeof(url), "https://%s%s", kApiHost, kApiPath);
  if (!http.begin(client, url)) {
    markCheckAttempt(false);
    client.stop();
    services::https::drainTlsHeapAfterSession();
    return false;
  }

  const int code = http.GET();
  if (code == HTTP_CODE_NOT_FOUND) {
    // The repo has no published release (the gift's fork has none): a normal
    // answer, so check again in a day, not every hour.
    http.end();
    client.stop();
    services::https::drainTlsHeapAfterSession();
    s_latest_tag[0] = '\0';
    s_asset_url[0] = '\0';
    s_available = false;
    markCheckAttempt(true);
    persistCache();
    Serial.println("[ota_gh] no release published (HTTP 404); next check in 24h");
    return true;
  }
  if (code != HTTP_CODE_OK) {
    Serial.printf("[ota_gh] releases HTTP %d (retry in %luh)\n", code,
                  static_cast<unsigned long>(kCheckRetryOnFailMs / 3600000UL));
    markCheckAttempt(false);
    http.end();
    client.stop();
    services::https::drainTlsHeapAfterSession();
    return false;
  }

  // Filter keeps tag + asset name/url only (release body can be large).
  JsonDocument filter;
  filter["tag_name"] = true;
  filter["assets"][0]["name"] = true;
  filter["assets"][0]["browser_download_url"] = true;

  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  client.stop();
  services::https::drainTlsHeapAfterSession();

  if (err) {
    Serial.printf("[ota_gh] json: %s\n", err.c_str());
    markCheckAttempt(false);
    return false;
  }

  const char* tag = doc["tag_name"] | "";
  if (tag[0] == '\0') {
    Serial.println("[ota_gh] missing tag_name");
    markCheckAttempt(false);
    return false;
  }

  const char* asset_url = nullptr;
  JsonArray assets = doc["assets"].as<JsonArray>();
  for (JsonObject asset : assets) {
    const char* name = asset["name"] | "";
    if (strcmp(name, kAppAssetName) == 0) {
      asset_url = asset["browser_download_url"] | "";
      break;
    }
  }
  if (asset_url == nullptr || asset_url[0] == '\0') {
    Serial.println("[ota_gh] app.bin asset not found");
    markCheckAttempt(false);
    return false;
  }

  strncpy(s_latest_tag, tag, sizeof(s_latest_tag) - 1);
  s_latest_tag[sizeof(s_latest_tag) - 1] = '\0';
  strncpy(s_asset_url, asset_url, sizeof(s_asset_url) - 1);
  s_asset_url[sizeof(s_asset_url) - 1] = '\0';
  s_available = isNewerThanRunning(s_latest_tag);
  markCheckAttempt(true);
  persistCache();
  Serial.printf("[ota_gh] latest=%s available=%d\n", s_latest_tag,
                s_available ? 1 : 0);
  return true;
}

void setInstallFailed(const char* msg) {
  strncpy(s_install_error, msg != nullptr ? msg : "failed",
          sizeof(s_install_error) - 1);
  s_install_error[sizeof(s_install_error) - 1] = '\0';
  s_install_pct = 0;
  s_install_bytes = 0;
  s_install_total = 0;
  s_install_state = InstallState::Failed;
}

void updateInstallProgress(size_t written, int content_len, size_t max_part) {
  s_install_bytes = static_cast<uint32_t>(written);
  size_t total = 0;
  if (content_len > 0) {
    total = static_cast<size_t>(content_len);
  } else if (max_part > 0) {
    // No Content-Length (common after GitHub redirects). Use a soft estimate so
    // the bar moves; clamp at 99 until Update.end succeeds.
    total = max_part > (6u * 1024u * 1024u) ? (5u * 1024u * 1024u + 512u * 1024u)
                                            : (max_part * 3u) / 4u;
  }
  s_install_total = static_cast<uint32_t>(total);
  if (total == 0) {
    s_install_pct = 0;
    return;
  }
  unsigned pct = static_cast<unsigned>((written * 100UL) / total);
  if (pct > 99) {
    pct = 99;
  }
  s_install_pct = static_cast<uint8_t>(pct);
}

/** The whole install. Returns normally on every path, so the HTTPS lock (ScopedLock),
 *  HTTPClient and TLS client destructors always run; the task deletes itself only
 *  after this returns. Calling vTaskDelete(nullptr) in here skipped them, so any
 *  failed install left the HTTPS lock held forever (no more planes) and leaked a
 *  socket. */
void runInstall() {
  char url[256];
  strncpy(url, s_asset_url, sizeof(url) - 1);
  url[sizeof(url) - 1] = '\0';

  if (url[0] == '\0') {
    setInstallFailed("no asset url");
    return;
  }

  services::https::ScopedLock tls(kInstallTimeoutMs);
  if (!tls.held()) {
    setInstallFailed("https busy");
    return;
  }
  services::https::drainTlsHeapAfterSession(600);

  services::https::SecureClient client;
  client.setInsecure();
  client.setTimeout(60);
  client.setHandshakeTimeout(30);

  HTTPClient http;
  // Per-read timeout. HTTPClient takes a uint16_t: the old 600000 ms wrapped to
  // ~10 s. The whole download is bounded by kInstallTimeoutMs in the loop below.
  http.setTimeout(30000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(kUserAgent);
  http.setReuse(false);

  // Closes the connection on every exit path below (runs before tls is released).
  struct SessionCleanup {
    HTTPClient& http;
    services::https::SecureClient& client;
    ~SessionCleanup() {
      http.end();
      client.stop();
      services::https::drainTlsHeapAfterSession();
    }
  } cleanup{http, client};

  if (!http.begin(client, url)) {
    setInstallFailed("http begin failed");
    return;
  }

  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    char msg[48];
    snprintf(msg, sizeof(msg), "download HTTP %d", code);
    setInstallFailed(msg);
    return;
  }

  const int content_len = http.getSize();
  const size_t max_part = otaAppPartitionSize();
  if (content_len > 0 &&
      !services::ota::firmwareSizeLooksValid(static_cast<size_t>(content_len), max_part)) {
    setInstallFailed("image size invalid");
    return;
  }

  if (!Update.begin(content_len > 0 ? static_cast<size_t>(content_len)
                                    : UPDATE_SIZE_UNKNOWN)) {
    setInstallFailed("Update.begin failed");
    return;
  }

  WiFiClient* stream = http.getStreamPtr();
  bool buf_psram = true;
  uint8_t* buf = static_cast<uint8_t*>(
      heap_caps_malloc(kStreamChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buf == nullptr) {
    buf_psram = false;
    buf = static_cast<uint8_t*>(malloc(kStreamChunk));
  }
  if (buf == nullptr || stream == nullptr) {
    Update.abort();
    if (buf != nullptr) {
      free(buf);
    }
    setInstallFailed("no buffer");
    return;
  }

  bool header_ok = false;
  size_t written = 0;
  size_t since_breather = 0;
  const unsigned long started = millis();
  bool failed = false;

  while (http.connected() && (content_len < 0 || written < static_cast<size_t>(content_len))) {
    if (millis() - started > kInstallTimeoutMs) {  // wrap-safe
      setInstallFailed("download timeout");
      failed = true;
      break;
    }
    const size_t avail = stream->available();
    if (avail == 0) {
      delay(1);
      continue;
    }
    const size_t to_read = avail > kStreamChunk ? kStreamChunk : avail;
    const int n = stream->readBytes(buf, to_read);
    if (n <= 0) {
      delay(1);
      continue;
    }
    if (!header_ok) {
      header_ok = true;
      if (!services::ota::firmwareHeaderLooksValid(
              buf, static_cast<size_t>(n),
              content_len > 0 ? static_cast<size_t>(content_len) : 0, max_part)) {
        setInstallFailed("invalid image header");
        failed = true;
        break;
      }
    }
    if (Update.write(buf, static_cast<size_t>(n)) != static_cast<size_t>(n)) {
      setInstallFailed("flash write failed");
      failed = true;
      break;
    }
    written += static_cast<size_t>(n);
    since_breather += static_cast<size_t>(n);
    updateInstallProgress(written, content_len, max_part);
    if (since_breather >= kOtaBreatherBytes) {
      since_breather = 0;
      delay(1);
    }
  }

  if (buf_psram) {
    heap_caps_free(buf);
  } else {
    free(buf);
  }

  if (failed) {
    Update.abort();
    return;
  }

  if (!services::ota::firmwareSizeLooksValid(written, max_part)) {
    Update.abort();
    setInstallFailed("final size invalid");
    return;
  }

  if (!Update.end(true)) {
    setInstallFailed("Update.end failed");
    return;
  }

  s_install_pct = 100;
  s_install_bytes = static_cast<uint32_t>(written);
  s_install_total = static_cast<uint32_t>(written);
  s_install_error[0] = '\0';
  s_install_state = InstallState::Succeeded;
  // After a successful install the running image is "current" until reboot —
  // clear the available flag so About stops nagging.
  s_available = false;
  persistCache();
  Serial.printf("[ota_gh] install ok %u bytes — reset device\n",
                static_cast<unsigned>(written));
}

void installTaskThunk(void*) {
  runInstall();  // every object in there is destroyed before the task ends
  if (s_install_state == InstallState::Failed) {
    Serial.printf("[ota_gh] install failed: %s (HTTPS lock released)\n", s_install_error);
  }
  s_install_task = nullptr;
  vTaskDelete(nullptr);
}

void checkJob() {
  fetchLatestFromGitHub();
  s_check_queued = false;
}

}  // namespace

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C) && !defined(FLIGHTSCNR_SIM)
// Arduino-ESP32 calls this from initArduino(). Returning true means the app itself
// decides when a freshly installed OTA image is good (confirmRunningImage); if it
// crashes or resets before that, the bootloader rolls back to the previous image.
// Needs CONFIG_APP_ROLLBACK_ENABLE (set in the P4 core's sdkconfig).
extern "C" bool verifyRollbackLater() { return true; }
#endif

void confirmRunningImage(const char* why) {
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C) && !defined(FLIGHTSCNR_SIM)
  static volatile bool s_done = false;
  if (s_done) {
    return;
  }
  s_done = true;
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  if (running == nullptr || esp_ota_get_state_partition(running, &state) != ESP_OK) {
    return;  // serial-flashed image: no OTA state, nothing to confirm
  }
  if (state != ESP_OTA_IMG_PENDING_VERIFY) {
    return;
  }
  if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
    Serial.printf("[ota_gh] new firmware confirmed (%s); rollback cancelled\n",
                  why != nullptr ? why : "");
  }
#else
  (void)why;
#endif
}

void init() {
  loadCache();
  // Session freshness only — always allow a check after boot.
  s_last_check_ms = 0;
}

void pollIfDue() {
  if (millis() >= kConfirmImageAfterMs) {
    confirmRunningImage("30 min uptime");
  }
  if (s_check_queued || s_install_state == InstallState::Running) {
    return;
  }
  if (millis() < kFirstCheckAfterBootMs || cacheFresh()) {
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }
  // Cheap when the worker's job slot is busy: just try again next pass.
  if (services::adsb::queueBackgroundJob(&checkJob)) {
    s_check_queued = true;
  }
}

bool checkLatest(bool force) {
  if (force && !s_check_queued && s_install_state != InstallState::Running &&
      WiFi.status() == WL_CONNECTED) {
    // Non-blocking: queue the check; the web page polls /ota/github/status.
    if (services::adsb::queueBackgroundJob(&checkJob)) {
      s_check_queued = true;
    }
  }
  return s_check_last_ok && cacheFresh();
}

bool checkInProgress() { return s_check_queued; }

bool updateAvailable() { return s_available && s_latest_tag[0] != '\0'; }

const char* latestTag() { return s_latest_tag; }

const char* currentVersion() { return config::kFirmwareVersion; }

bool startInstall() {
  if (s_install_state == InstallState::Running || s_install_task != nullptr) {
    return false;
  }
  if (WiFi.status() != WL_CONNECTED) {
    setInstallFailed("wifi down");
    return false;
  }
  if (s_asset_url[0] == '\0') {
    checkLatest(true);  // non-blocking: queues a check for the next try
    setInstallFailed(s_check_queued ? "checking GitHub, try again in a moment"
                                    : "no release asset");
    return false;
  }
  if (s_check_queued) {
    setInstallFailed("checking GitHub, try again in a moment");
    return false;
  }
  s_install_error[0] = '\0';
  s_install_pct = 0;
  s_install_bytes = 0;
  s_install_total = 0;
  s_install_state = InstallState::Running;

  // Same stack as the ADS-B HTTPS worker. 8 KB overflows inside mbedTLS
  // during the GitHub download and the chip resets before progress can be read.
  const BaseType_t ok =
      xTaskCreatePinnedToCore(installTaskThunk, "ota_gh", 16384, nullptr, 1,
                              &s_install_task, config::kCoreNetwork);
  if (ok != pdPASS) {
    s_install_task = nullptr;
    setInstallFailed("task create failed");
    return false;
  }
  return true;
}

InstallState installState() { return s_install_state; }

uint8_t installPercent() { return s_install_pct; }

uint32_t installBytes() { return s_install_bytes; }

uint32_t installTotal() { return s_install_total; }

const char* installError() { return s_install_error; }

}  // namespace services::ota_github
