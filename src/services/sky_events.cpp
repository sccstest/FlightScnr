#include "services/sky_events.h"

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "config.h"
#include "hardware/buzzer.h"
#include "services/adsb_client.h"
#include "services/clock_time.h"
#include "services/https_heap.h"
#include "services/https_lock.h"
#include "services/map_center.h"
#include "services/off_hours.h"
#include "services/route_cache_store.h"
#include "services/secure_client.h"
#include "services/settings_state.h"

namespace services::sky {
namespace {

// --- Settings (toggles live with the other settings; caches stay on LittleFS) ------
constexpr char kStoreNs[] = "flightscnr";
constexpr char kLaunchKey[] = "sky_launch";
constexpr char kIssKey[] = "sky_iss";

// --- Sources -------------------------------------------------------------------------
// Launch Library 2: upcoming launches from Cape Canaveral SFS (location 12) and
// Kennedy Space Center (27). "normal" mode is needed for pad + rocket names.
constexpr char kLl2Url[] =
    "https://ll.thespacedevs.com/2.3.0/launches/upcoming/"
    "?limit=3&location__ids=12,27&hide_recent_previous=true";
constexpr char kTleUrl[] = "https://celestrak.org/NORAD/elements/gp.php?CATNR=25544&FORMAT=TLE";
constexpr char kUserAgent[] = "FlightScnr/" FLIGHTSCNR_FIRMWARE_VERSION
                              " (desk radar sky events; +https://github.com/yashmulgaonkar/FlightScnr)";
constexpr uint32_t kHttpTimeoutMs = 12000;
constexpr int kMaxBodyBytes = 96 * 1024;

constexpr char kLaunchCachePath[] = "/sky_ll2.bin";
constexpr char kTleCachePath[] = "/sky_tle.txt";
constexpr uint32_t kLaunchCacheMagic = 0x534B5931;  // "SKY1"

// Poll pacing (LL2 free tier is ~15 requests per hour).
constexpr int64_t kLaunchPollIdleS = 60 * 60;
constexpr int64_t kLaunchPollSoonS = 15 * 60;    // launch within 2 h
constexpr int64_t kLaunchPollFinalS = 5 * 60;    // T-20 min .. T+15 min
constexpr int64_t kLaunchRetryS = 15 * 60;
constexpr int64_t kLaunchRateLimitedS = 60 * 60;
constexpr int64_t kTleRefreshS = 24 * 3600;
constexpr int64_t kTleRetryS = 60 * 60;
constexpr int64_t kTleMaxAgeDays = 14;            // older elements: stop predicting

// Banner / alarm timing.
constexpr int64_t kLaunchBannerLeadS = 24 * 3600;
constexpr int64_t kLaunchAlarmLeadS = 10 * 60;
constexpr int64_t kLaunchLookWindowS = 10 * 60;   // "look east" from T-10 min
constexpr int64_t kLaunchAfterS = 8 * 60;         // banner stays up after T-0
constexpr int64_t kIssBannerLeadS = 45 * 60;
constexpr int64_t kIssAlarmLeadS = 2 * 60;
constexpr float kIssMinPeakDeg = 20.0f;
constexpr int64_t kPassSearchSpanS = 3 * 24 * 3600;
constexpr int64_t kPassSearchEveryS = 6 * 3600;
constexpr size_t kMaxPasses = 6;
constexpr size_t kMaxLaunches = 3;

bool s_launch_enabled = config::kFactorySkyLaunchAlerts;
bool s_iss_enabled = config::kFactorySkyIssAlerts;

Launch s_launches[kMaxLaunches];
size_t s_launch_count = 0;
int64_t s_launch_fetched_at = 0;  // unix s of the last good LL2 reply (0 = never)
int64_t s_launch_next_poll = 0;   // unix s
int s_launch_last_http = 0;

char s_tle1[72] = "";
char s_tle2[72] = "";
int64_t s_tle_fetched_at = 0;
int64_t s_tle_next_poll = 0;
orbit::Sgp4 s_sat;
bool s_sat_ok = false;

orbit::Pass s_passes[kMaxPasses];
size_t s_pass_count = 0;
orbit::Pass s_pass_work[kMaxPasses];
size_t s_pass_work_count = 0;
orbit::PassFinder s_finder;
bool s_search_running = false;
int64_t s_search_started_at = 0;
double s_search_lat = 999.0;
double s_search_lon = 999.0;
bool s_fake_pass = false;

Banner s_banner;
const Launch* s_banner_launch = nullptr;

// Alarm bookkeeping: what was already announced (re-arms when the time changes).
char s_alarm_t10_id[16] = "";
int64_t s_alarm_t10_net = 0;
char s_alarm_lift_id[16] = "";
int64_t s_alarm_lift_net = 0;
int64_t s_alarm_iss_start = 0;

// --- Background job hand-off (job runs on the ADS-B worker, core 0) ----------------
enum class Job : uint8_t { None, Launches, Tle };
volatile Job s_job = Job::None;
volatile bool s_job_done = false;
volatile bool s_job_ok = false;
volatile int s_job_http = 0;
Launch s_job_launches[kMaxLaunches];
size_t s_job_launch_count = 0;
char s_job_tle1[72] = "";
char s_job_tle2[72] = "";

unsigned long s_last_tick_ms = 0;

int64_t nowUnix() {
  const time_t t = time(nullptr);
  return t > 1700000000 ? static_cast<int64_t>(t) : 0;
}

void copyStr(char* dst, size_t len, const char* src) {
  if (len == 0) return;
  snprintf(dst, len, "%s", src != nullptr ? src : "");
}

int64_t parseIso(const char* s) {
  if (s == nullptr) return 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
  if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return 0;
  // days_from_civil
  y -= mo <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;
  return days * 86400 + h * 3600 + mi * 60 + se;
}

void eraseAll(char* s, const char* what) {
  const size_t n = strlen(what);
  for (char* p = strstr(s, what); p != nullptr; p = strstr(p, what)) {
    memmove(p, p + n, strlen(p + n) + 1);
  }
}

void replaceFirst(char* s, size_t cap, const char* what, const char* with) {
  char* p = strstr(s, what);
  if (p == nullptr) return;
  char tail[64];
  snprintf(tail, sizeof(tail), "%s", p + strlen(what));
  snprintf(p, cap - static_cast<size_t>(p - s), "%s%s", with, tail);
}

/** "Starlink Group 12-5" -> "Starlink 12-5": the banner is narrow. */
void tidyMission(char* m) { eraseAll(m, "Group "); }

/** "Space Launch Complex 40" -> "SLC-40", "Launch Complex 39A" -> "LC-39A". */
void tidyPad(char* p, size_t cap) {
  replaceFirst(p, cap, "Space Launch Complex ", "SLC-");
  replaceFirst(p, cap, "Launch Complex ", "LC-");
}

/** "Cape Canaveral SFS, FL, USA" -> "Cape Canaveral SFS". */
void tidySite(char* s) {
  char* c = strchr(s, ',');
  if (c != nullptr) *c = 0;
}

bool statusLooksGo(uint8_t id) { return id == 1 || id == 6 || id == 8; }

// --- HTTPS ---------------------------------------------------------------------------

/** GET url into body. Returns the HTTP code (or a negative client error). */
int httpsGet(const char* url, String* body) {
  services::https::ScopedLock tls(kHttpTimeoutMs + 4000);
  if (!tls.held()) {
    Serial.println("[sky] HTTPS busy (skipped)");
    return -100;
  }
  services::https::SecureClient client;
  client.setInsecure();
  client.setTimeout(kHttpTimeoutMs / 1000UL);
  client.setHandshakeTimeout(kHttpTimeoutMs / 1000UL);
  HTTPClient http;
  if (!http.begin(client, url)) {
    return -101;
  }
  http.setConnectTimeout(kHttpTimeoutMs);
  http.setTimeout(kHttpTimeoutMs);
  http.setReuse(false);
  http.setUserAgent(kUserAgent);
  const int code = http.GET();
  if (code == HTTP_CODE_OK) {
    const int size = http.getSize();
    if (size > kMaxBodyBytes) {
      Serial.printf("[sky] body too large (%d)\n", size);
      http.end();
      client.stop();
      services::https::drainTlsHeapAfterSession();
      return -102;
    }
    *body = http.getString();  // de-chunks (LL2 replies are chunked)
  }
  http.end();
  client.stop();
  services::https::drainTlsHeapAfterSession();
  return code;
}

bool parseLaunches(const String& body, Launch* out, size_t* count) {
  JsonDocument filter;
  JsonObject f = filter["results"][0].to<JsonObject>();
  f["id"] = true;
  f["net"] = true;
  f["window_start"] = true;
  f["window_end"] = true;
  f["status"]["id"] = true;
  f["status"]["name"] = true;
  f["net_precision"]["id"] = true;
  f["rocket"]["configuration"]["name"] = true;
  f["mission"]["name"] = true;
  f["pad"]["name"] = true;
  f["pad"]["latitude"] = true;
  f["pad"]["longitude"] = true;
  f["pad"]["location"]["name"] = true;
  JsonDocument doc;
  const DeserializationError err =
      deserializeJson(doc, body, DeserializationOption::Filter(filter),
                      DeserializationOption::NestingLimit(32));
  if (err) {
    Serial.printf("[sky] LL2 JSON: %s\n", err.c_str());
    return false;
  }
  JsonArrayConst results = doc["results"].as<JsonArrayConst>();
  if (results.isNull()) return false;
  size_t n = 0;
  for (JsonObjectConst r : results) {
    if (n >= kMaxLaunches) break;
    Launch l;
    copyStr(l.id, sizeof(l.id), r["id"] | "");
    copyStr(l.rocket, sizeof(l.rocket), r["rocket"]["configuration"]["name"] | "Rocket");
    copyStr(l.mission, sizeof(l.mission), r["mission"]["name"] | "");
    tidyMission(l.mission);
    copyStr(l.pad, sizeof(l.pad), r["pad"]["name"] | "");
    tidyPad(l.pad, sizeof(l.pad));
    copyStr(l.site, sizeof(l.site), r["pad"]["location"]["name"] | "");
    tidySite(l.site);
    copyStr(l.status, sizeof(l.status), r["status"]["name"] | "");
    l.status_id = r["status"]["id"] | 0;
    l.precision = r["net_precision"]["id"].isNull() ? 255 : (r["net_precision"]["id"] | 255);
    l.net = parseIso(r["net"] | "");
    l.window_start = parseIso(r["window_start"] | "");
    l.window_end = parseIso(r["window_end"] | "");
    // LL2 sends the pad coordinates as strings in some versions, numbers in others.
    JsonVariantConst la = r["pad"]["latitude"];
    JsonVariantConst lo = r["pad"]["longitude"];
    l.pad_lat = la.is<const char*>() ? static_cast<float>(atof(la.as<const char*>())) : la.as<float>();
    l.pad_lon = lo.is<const char*>() ? static_cast<float>(atof(lo.as<const char*>())) : lo.as<float>();
    if (l.pad_lat == 0.0f && l.pad_lon == 0.0f) {
      l.pad_lat = 28.5f;  // Cape Canaveral, near enough for "which way to look"
      l.pad_lon = -80.58f;
    }
    if (l.net == 0) continue;
    out[n++] = l;
  }
  *count = n;
  return true;
}

void skyJob() {
  const Job job = s_job;
  String body;
  if (job == Job::Launches) {
    const int code = httpsGet(kLl2Url, &body);
    s_job_http = code;
    size_t n = 0;
    s_job_ok = code == HTTP_CODE_OK && parseLaunches(body, s_job_launches, &n);
    s_job_launch_count = s_job_ok ? n : 0;
    Serial.printf("[sky] LL2 HTTP %d -> %u launches (%u bytes)\n", code,
                  static_cast<unsigned>(s_job_launch_count), static_cast<unsigned>(body.length()));
  } else if (job == Job::Tle) {
    const int code = httpsGet(kTleUrl, &body);
    s_job_http = code;
    s_job_ok = false;
    if (code == HTTP_CODE_OK) {
      const int i1 = body.indexOf("\n1 ");
      const int i2 = body.indexOf("\n2 ");
      if (i1 >= 0 && i2 > i1) {
        copyStr(s_job_tle1, sizeof(s_job_tle1), body.substring(i1 + 1, i1 + 70).c_str());
        copyStr(s_job_tle2, sizeof(s_job_tle2), body.substring(i2 + 1, i2 + 70).c_str());
        orbit::Tle t;
        s_job_ok = orbit::parseTle(s_job_tle1, s_job_tle2, &t);
      }
    }
    Serial.printf("[sky] TLE HTTP %d ok=%d\n", code, s_job_ok ? 1 : 0);
  }
  s_job_done = true;
}

// --- Caches (LittleFS, not NVS) --------------------------------------------------------

struct LaunchCacheHeader {
  uint32_t magic;
  uint32_t count;
  int64_t fetched_at;
  uint32_t record_size;
};

void saveLaunchCache() {
  if (!services::route_cache::mount()) return;
  File f = LittleFS.open(kLaunchCachePath, "w");
  if (!f) return;
  size_t n = 0;
  Launch real[kMaxLaunches];
  for (size_t i = 0; i < s_launch_count; ++i) {
    if (!s_launches[i].fake) real[n++] = s_launches[i];
  }
  LaunchCacheHeader h{kLaunchCacheMagic, static_cast<uint32_t>(n), s_launch_fetched_at,
                      static_cast<uint32_t>(sizeof(Launch))};
  f.write(reinterpret_cast<const uint8_t*>(&h), sizeof(h));
  f.write(reinterpret_cast<const uint8_t*>(real), n * sizeof(Launch));
  f.close();
}

void loadLaunchCache() {
  if (!services::route_cache::mount() || !LittleFS.exists(kLaunchCachePath)) return;
  File f = LittleFS.open(kLaunchCachePath, "r");
  if (!f) return;
  LaunchCacheHeader h{};
  if (f.read(reinterpret_cast<uint8_t*>(&h), sizeof(h)) == sizeof(h) &&
      h.magic == kLaunchCacheMagic && h.record_size == sizeof(Launch) &&
      h.count <= kMaxLaunches) {
    const size_t want = h.count * sizeof(Launch);
    if (f.read(reinterpret_cast<uint8_t*>(s_launches), want) == want) {
      s_launch_count = h.count;
      s_launch_fetched_at = h.fetched_at;
      Serial.printf("[sky] launch cache: %u launches\n", static_cast<unsigned>(s_launch_count));
    }
  }
  f.close();
}

void saveTleCache() {
  if (!services::route_cache::mount()) return;
  File f = LittleFS.open(kTleCachePath, "w");
  if (!f) return;
  char head[24];
  snprintf(head, sizeof(head), "%lld\n", static_cast<long long>(s_tle_fetched_at));
  f.print(head);
  f.print(s_tle1);
  f.print("\n");
  f.print(s_tle2);
  f.print("\n");
  f.close();
}

bool applyTle(const char* l1, const char* l2) {
  orbit::Tle t;
  if (!orbit::parseTle(l1, l2, &t) || !orbit::sgp4Init(t, &s_sat)) return false;
  copyStr(s_tle1, sizeof(s_tle1), l1);
  copyStr(s_tle2, sizeof(s_tle2), l2);
  s_sat_ok = true;
  s_search_running = false;
  s_search_started_at = 0;  // re-predict with the new elements
  return true;
}

void loadTleCache() {
  if (!services::route_cache::mount() || !LittleFS.exists(kTleCachePath)) return;
  File f = LittleFS.open(kTleCachePath, "r");
  if (!f) return;
  const String head = f.readStringUntil('\n');
  String l1 = f.readStringUntil('\n');
  String l2 = f.readStringUntil('\n');
  f.close();
  l1.trim();
  l2.trim();
  if (applyTle(l1.c_str(), l2.c_str())) {
    s_tle_fetched_at = atoll(head.c_str());
    Serial.println("[sky] TLE cache loaded");
  }
}

// --- Polling --------------------------------------------------------------------------

/** Next launch worth watching closely (soonest that isn't over). */
const Launch* primaryLaunch(int64_t now) {
  const Launch* best = nullptr;
  for (size_t i = 0; i < s_launch_count; ++i) {
    const Launch& l = s_launches[i];
    if (l.status_id == 4 || l.status_id == 7) continue;  // failure
    if (l.net < now - kLaunchAfterS) continue;
    if (best == nullptr || l.net < best->net) best = &l;
  }
  return best;
}

int64_t launchPollInterval(int64_t now) {
  const Launch* l = primaryLaunch(now);
  if (l == nullptr || l->fake) return kLaunchPollIdleS;
  const int64_t dt = l->net - now;
  if (dt <= 20 * 60 && dt >= -15 * 60) return kLaunchPollFinalS;
  if (dt <= 2 * 3600) return kLaunchPollSoonS;
  return kLaunchPollIdleS;
}

void applyLaunchResult(int64_t now) {
  // Keep a launch we were counting down if it dropped out of "upcoming" right
  // after liftoff, so "Liftoff!" still shows for a few minutes. Keep sim fakes too.
  Launch keep[kMaxLaunches];
  size_t keep_n = 0;
  for (size_t i = 0; i < s_launch_count && keep_n < kMaxLaunches; ++i) {
    const Launch& old = s_launches[i];
    bool present = false;
    for (size_t j = 0; j < s_job_launch_count; ++j) {
      if (strcmp(old.id, s_job_launches[j].id) == 0) present = true;
    }
    const bool just_launched =
        !present && statusLooksGo(old.status_id) && old.net <= now + 60 && old.net > now - kLaunchAfterS;
    if (old.fake || just_launched) keep[keep_n++] = old;
  }
  size_t n = 0;
  for (size_t i = 0; i < keep_n; ++i) s_launches[n++] = keep[i];
  for (size_t j = 0; j < s_job_launch_count && n < kMaxLaunches; ++j) {
    s_launches[n++] = s_job_launches[j];
  }
  s_launch_count = n;
  s_launch_fetched_at = now;
  saveLaunchCache();
  for (size_t i = 0; i < s_launch_count; ++i) {
    const Launch& l = s_launches[i];
    Serial.printf("[sky] launch %s | %s | %s | status %u %s | T%+lld s\n", l.rocket, l.mission,
                  l.pad, l.status_id, l.status, static_cast<long long>(now - l.net));
  }
}

void tickFetches(int64_t now) {
  if (s_job_done) {
    const Job job = s_job;
    const bool ok = s_job_ok;
    s_job_done = false;
    s_job = Job::None;
    if (job == Job::Launches) {
      s_launch_last_http = s_job_http;
      if (ok) {
        applyLaunchResult(now);
        s_launch_next_poll = now + launchPollInterval(now);
      } else {
        s_launch_next_poll = now + (s_job_http == 429 ? kLaunchRateLimitedS : kLaunchRetryS);
      }
    } else if (job == Job::Tle) {
      if (ok && applyTle(s_job_tle1, s_job_tle2)) {
        s_tle_fetched_at = now;
        s_tle_next_poll = now + kTleRefreshS;
        saveTleCache();
      } else {
        s_tle_next_poll = now + kTleRetryS;
      }
    }
  }
  if (s_job != Job::None || WiFi.status() != WL_CONNECTED || now == 0) return;
  if (services::adsb::backgroundJobActive()) return;

  Job want = Job::None;
  if (s_launch_enabled && now >= s_launch_next_poll) {
    want = Job::Launches;
  } else if (s_iss_enabled && now >= s_tle_next_poll) {
    want = Job::Tle;
  }
  if (want == Job::None) return;
  s_job = want;
  s_job_done = false;
  if (!services::adsb::queueBackgroundJob(&skyJob)) {
    s_job = Job::None;  // worker busy: try again on a later tick
    return;
  }
  Serial.printf("[sky] queued %s fetch\n", want == Job::Launches ? "launch" : "TLE");
}

// --- ISS pass prediction --------------------------------------------------------------

void tickPassSearch(int64_t now) {
  if (!s_iss_enabled || !s_sat_ok || now == 0) return;
  // Drop passes that are over (fakes too).
  size_t w = 0;
  for (size_t i = 0; i < s_pass_count; ++i) {
    if (s_passes[i].end >= now) s_passes[w++] = s_passes[i];
  }
  if (w == 0) s_fake_pass = false;
  s_pass_count = w;

  const double lat = services::map_center::latitude();
  const double lon = services::map_center::longitude();
  const bool moved = std::fabs(lat - s_search_lat) > 0.01 || std::fabs(lon - s_search_lon) > 0.01;
  const double tle_age_days = (orbit::unixToJd(static_cast<double>(now)) - s_sat.tle.epoch_jd);
  if (tle_age_days > kTleMaxAgeDays) {
    if (!s_fake_pass) s_pass_count = 0;
    return;
  }
  if (!s_search_running && (moved || s_search_started_at == 0 ||
                            now - s_search_started_at >= kPassSearchEveryS)) {
    s_search_running = true;
    s_search_started_at = now;
    s_search_lat = lat;
    s_search_lon = lon;
    s_pass_work_count = 0;
    s_finder.begin(&s_sat, lat, lon, now, now + kPassSearchSpanS);
  }
  if (!s_search_running) return;
  // A slice per loop: ~30 SGP4 evaluations, plus a fine scan when a pass is found.
  if (s_finder.step(30)) {
    const orbit::Pass& p = s_finder.pass();
    if (p.peak_el >= kIssMinPeakDeg && p.end >= now && s_pass_work_count < kMaxPasses) {
      s_pass_work[s_pass_work_count++] = p;
    }
  }
  if (s_finder.done() || s_pass_work_count >= kMaxPasses) {
    s_search_running = false;
    if (!s_fake_pass) {
      memcpy(s_passes, s_pass_work, sizeof(orbit::Pass) * s_pass_work_count);
      s_pass_count = s_pass_work_count;
    }
    Serial.printf("[sky] ISS: %u visible passes (>=%.0f deg) in the next 3 days\n",
                  static_cast<unsigned>(s_pass_work_count), kIssMinPeakDeg);
    for (size_t i = 0; i < s_pass_work_count; ++i) {
      char t[16];
      services::clock::formatClockFromEpoch(s_pass_work[i].start, t, sizeof(t));
      Serial.printf("[sky]   %s  %s->%s  peak %.0f deg (%lld min from now)\n", t,
                    orbit::compass8(s_pass_work[i].start_az), orbit::compass8(s_pass_work[i].end_az),
                    s_pass_work[i].peak_el,
                    static_cast<long long>((s_pass_work[i].start - now) / 60));
    }
  }
}

const orbit::Pass* nextPass(int64_t now) {
  for (size_t i = 0; i < s_pass_count; ++i) {
    if (s_passes[i].end >= now) return &s_passes[i];
  }
  return nullptr;
}

// --- Banner + alarms --------------------------------------------------------------------

void formatCountdown(int64_t dt, char* out, size_t len) {
  if (dt < 0) dt = 0;
  const int h = static_cast<int>(dt / 3600);
  const int m = static_cast<int>((dt / 60) % 60);
  const int s = static_cast<int>(dt % 60);
  if (h > 0) {
    snprintf(out, len, "T-%d:%02d:%02d", h, m, s);
  } else {
    snprintf(out, len, "T-%d:%02d", m, s);
  }
}

void playAlarm(hardware::chime::Kind kind, const char* why) {
  Serial.printf("[sky] alarm: %s\n", why);
  if (services::offhours::active()) return;  // night mode: banner only
  hardware::buzzerAlert(kind);
}

/** Fill b for the launch; returns its "time to event" (0 when happening now). */
int64_t buildLaunchBanner(const Launch& l, int64_t now, Banner* b) {
  const int64_t dt = l.net - now;
  float bearing = 90.0f;
  launchBearing(l, &bearing, nullptr);
  const char* dir = compassWord(bearing);
  b->kind = EventKind::Launch;
  char when[16];
  services::clock::formatClockFromEpoch(l.net, when, sizeof(when));
  if (l.status_id == 5) {  // hold
    snprintf(b->title, sizeof(b->title), "%s|%s", l.rocket, l.mission);
    snprintf(b->detail, sizeof(b->detail), dt > 0 ? "Hold  NET %s" : "Hold", when);
    return dt > 0 ? dt : 0;
  }
  if (l.status_id == 2) {  // date not firm yet
    snprintf(b->title, sizeof(b->title), "%s|%s", l.rocket, l.mission);
    snprintf(b->detail, sizeof(b->detail), "NET %s (TBD)", when);
    return dt > 0 ? dt : 0;
  }
  if (dt > kLaunchLookWindowS) {
    snprintf(b->title, sizeof(b->title), "%s|%s", l.rocket, l.mission);
    formatCountdown(dt, b->detail, sizeof(b->detail));
    return dt;
  }
  if (dt > 0) {
    snprintf(b->title, sizeof(b->title), "%s|Look %s", l.rocket, dir);
    formatCountdown(dt, b->detail, sizeof(b->detail));
    b->urgent = true;
    return dt;
  }
  snprintf(b->title, sizeof(b->title), "%s|Liftoff!", l.rocket);
  snprintf(b->detail, sizeof(b->detail), "Look %s, low", dir);
  b->urgent = true;
  return 0;
}

int64_t buildIssBanner(const orbit::Pass& p, int64_t now, Banner* b) {
  b->kind = EventKind::Iss;
  char when[16];
  services::clock::formatClockFromEpoch(p.start, when, sizeof(when));
  if (now >= p.start) {
    float az = p.peak_az;
    float el = p.peak_el;
    issLookNow(&az, &el);
    snprintf(b->title, sizeof(b->title), "ISS overhead|now");
    const char* height = el >= 60.0f ? "very high" : el >= 30.0f ? "high" : "low";
    snprintf(b->detail, sizeof(b->detail), "Look %s, %s", orbit::compass8(az), height);
    b->urgent = true;
    return 0;
  }
  const int64_t dt = p.start - now;
  if (dt <= kIssAlarmLeadS + 60) {
    snprintf(b->title, sizeof(b->title), "ISS overhead|in %d:%02d", static_cast<int>(dt / 60),
             static_cast<int>(dt % 60));
    b->urgent = true;
  } else {
    snprintf(b->title, sizeof(b->title), "ISS overhead|%s", when);
  }
  snprintf(b->detail, sizeof(b->detail), "Look %s to %s", orbit::compass8(p.start_az),
           orbit::compass8(p.end_az));
  return dt;
}

void tickBannerAndAlarms(int64_t now) {
  Banner launch_b;
  Banner iss_b;
  int64_t launch_dt = -1;
  int64_t iss_dt = -1;
  const Launch* l = (s_launch_enabled && now != 0) ? primaryLaunch(now) : nullptr;
  if (l != nullptr) {
    const int64_t dt = l->net - now;
    const bool precise = l->precision <= 2 || l->precision == 255;
    if (precise && dt <= kLaunchBannerLeadS) {
      launch_dt = buildLaunchBanner(*l, now, &launch_b);
    }
    // Alarms: T-10 min and liftoff, only for launches that are "Go" (re-arm on a new T-0).
    if (statusLooksGo(l->status_id) && precise) {
      const bool t10_done = strcmp(s_alarm_t10_id, l->id) == 0 && s_alarm_t10_net == l->net;
      if (!t10_done && dt <= kLaunchAlarmLeadS && dt > 60) {
        copyStr(s_alarm_t10_id, sizeof(s_alarm_t10_id), l->id);
        s_alarm_t10_net = l->net;
        playAlarm(hardware::chime::Kind::Rocket, "launch T-10 min");
      }
      const bool lift_done = strcmp(s_alarm_lift_id, l->id) == 0 && s_alarm_lift_net == l->net;
      if (!lift_done && dt <= 0 && dt > -120) {
        copyStr(s_alarm_lift_id, sizeof(s_alarm_lift_id), l->id);
        s_alarm_lift_net = l->net;
        playAlarm(hardware::chime::Kind::Rocket, "liftoff");
      }
    }
  }
  const orbit::Pass* p = (s_iss_enabled && now != 0) ? nextPass(now) : nullptr;
  if (p != nullptr && p->start - now <= kIssBannerLeadS) {
    iss_dt = buildIssBanner(*p, now, &iss_b);
    if (s_alarm_iss_start != p->start && p->start - now <= kIssAlarmLeadS && now < p->end) {
      s_alarm_iss_start = p->start;
      playAlarm(hardware::chime::Kind::Satellite, "ISS pass");
    }
  }
  // One banner: whichever event is sooner (happening now = 0); a tie goes to the launch.
  if (launch_dt >= 0 && (iss_dt < 0 || launch_dt <= iss_dt)) {
    s_banner = launch_b;
    s_banner_launch = l;
  } else if (iss_dt >= 0) {
    s_banner = iss_b;
    s_banner_launch = nullptr;
  } else {
    s_banner = Banner{};
    s_banner_launch = nullptr;
  }
}

// --- Simulator test hooks ----------------------------------------------------------------
#if defined(FLIGHTSCNR_SIM)
float s_fake_launch_at_s = -1, s_fake_launch_min = 0;
float s_fake_iss_at_s = -1, s_fake_iss_min = 0;

void parseFakeArgs() {
  for (int i = 1; i + 1 < __argc; ++i) {
    float a = 0, b = 0;
    if (sscanf(__argv[i + 1], "%f:%f", &a, &b) != 2) continue;
    if (strcmp(__argv[i], "--sky-fake-launch") == 0) {
      s_fake_launch_at_s = a;
      s_fake_launch_min = b;
    } else if (strcmp(__argv[i], "--sky-fake-iss") == 0) {
      s_fake_iss_at_s = a;
      s_fake_iss_min = b;
    }
  }
}

void tickFakes(int64_t now) {
  if (now == 0) return;
  const float up_s = millis() / 1000.0f;
  if (s_fake_launch_at_s >= 0 && up_s >= s_fake_launch_at_s) {
    s_fake_launch_at_s = -1;
    Launch f;
    copyStr(f.id, sizeof(f.id), "sim-fake");
    copyStr(f.rocket, sizeof(f.rocket), "Falcon 9");
    copyStr(f.mission, sizeof(f.mission), "Starlink 12-5");
    copyStr(f.pad, sizeof(f.pad), "SLC-40");
    copyStr(f.site, sizeof(f.site), "Cape Canaveral SFS");
    copyStr(f.status, sizeof(f.status), "Go for Launch");
    f.status_id = 1;
    f.precision = 1;
    f.net = now + static_cast<int64_t>(s_fake_launch_min * 60.0f);
    f.window_start = f.net;
    f.window_end = f.net + 4 * 3600;
    f.pad_lat = 28.5619f;
    f.pad_lon = -80.5772f;
    f.fake = true;
    for (size_t i = (s_launch_count < kMaxLaunches ? s_launch_count : kMaxLaunches - 1); i > 0; --i) {
      s_launches[i] = s_launches[i - 1];
    }
    s_launches[0] = f;
    if (s_launch_count < kMaxLaunches) ++s_launch_count;
    Serial.printf("[sky] sim: fake launch at T-%.1f min\n", s_fake_launch_min);
  }
  if (s_fake_iss_at_s >= 0 && up_s >= s_fake_iss_at_s) {
    s_fake_iss_at_s = -1;
    orbit::Pass p;
    p.start = now + static_cast<int64_t>(s_fake_iss_min * 60.0f);
    p.peak = p.start + 180;
    p.end = p.start + 360;
    p.start_az = 315;  // NW, past the SW, to SE
    p.peak_az = 225;
    p.end_az = 135;
    p.peak_el = 58;
    s_passes[0] = p;
    s_pass_count = 1;
    s_fake_pass = true;
    Serial.printf("[sky] sim: fake ISS pass in %.1f min\n", s_fake_iss_min);
  }
}
#endif

void persist(const char* key, bool v) {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(key, v);
    prefs.end();
  }
  settingsStateBump();
}

}  // namespace

void bootLoad() {
  Preferences prefs;
  if (prefs.begin(kStoreNs, true)) {
    s_launch_enabled = prefs.getBool(kLaunchKey, config::kFactorySkyLaunchAlerts);
    s_iss_enabled = prefs.getBool(kIssKey, config::kFactorySkyIssAlerts);
    prefs.end();
  }
  loadLaunchCache();
  loadTleCache();
#if defined(FLIGHTSCNR_SIM)
  parseFakeArgs();
#endif
  Serial.printf("[sky] boot launches=%d iss=%d\n", s_launch_enabled ? 1 : 0, s_iss_enabled ? 1 : 0);
}

void tick() {
  const unsigned long ms = millis();
  // Polls / prediction / banner need ~4 Hz at most (the countdown shows seconds).
  if (ms - s_last_tick_ms < 250) return;
  s_last_tick_ms = ms;
  const int64_t now = nowUnix();
  if (now != 0) {
    // First tick with a valid clock: schedule polls from the cache ages.
    static bool s_scheduled = false;
    if (!s_scheduled) {
      s_scheduled = true;
      s_launch_next_poll =
          s_launch_fetched_at > 0 && s_launch_fetched_at <= now
              ? s_launch_fetched_at + launchPollInterval(now)
              : now;
      s_tle_next_poll = s_tle_fetched_at > 0 && s_tle_fetched_at <= now
                            ? s_tle_fetched_at + kTleRefreshS
                            : now;
    }
  }
#if defined(FLIGHTSCNR_SIM)
  tickFakes(now);
#endif
  tickFetches(now);
  tickPassSearch(now);
  tickBannerAndAlarms(now);
}

bool launchesEnabled() { return s_launch_enabled; }
bool issEnabled() { return s_iss_enabled; }

void toggleLaunches() {
  s_launch_enabled = !s_launch_enabled;
  persist(kLaunchKey, s_launch_enabled);
}

void toggleIss() {
  s_iss_enabled = !s_iss_enabled;
  persist(kIssKey, s_iss_enabled);
}

void saveFromForm(const char* launch_checkbox, const char* iss_checkbox) {
  s_launch_enabled = launch_checkbox != nullptr && launch_checkbox[0] == 'T';
  s_iss_enabled = iss_checkbox != nullptr && iss_checkbox[0] == 'T';
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kLaunchKey, s_launch_enabled);
    prefs.putBool(kIssKey, s_iss_enabled);
    prefs.end();
  }
}

const Banner& banner() { return s_banner; }

const Launch* currentLaunch() {
  if (s_banner_launch != nullptr) return s_banner_launch;
  return primaryLaunch(nowUnix());
}

const orbit::Pass* issPasses(size_t* count) {
  if (count != nullptr) *count = s_pass_count;
  return s_passes;
}

bool issLookNow(float* az, float* el) {
  const int64_t now = nowUnix();
  const orbit::Pass* p = nextPass(now);
  if (p == nullptr || now < p->start || now > p->end) return false;
  if (s_fake_pass) {
    // Simulator pass: ease along start -> peak -> end.
    const bool first = now < p->peak;
    const float f = first ? static_cast<float>(now - p->start) / static_cast<float>(p->peak - p->start)
                          : static_cast<float>(now - p->peak) / static_cast<float>(p->end - p->peak);
    *az = first ? p->start_az + (p->peak_az - p->start_az) * f
                : p->peak_az + (p->end_az - p->peak_az) * f;
    *el = first ? 10.0f + (p->peak_el - 10.0f) * f : p->peak_el - (p->peak_el - 10.0f) * f;
    return true;
  }
  orbit::Look look;
  if (!s_sat_ok || !orbit::lookAngles(s_sat, static_cast<double>(now),
                                      services::map_center::latitude(),
                                      services::map_center::longitude(), 0, &look)) {
    return false;
  }
  *az = look.az_deg;
  *el = look.el_deg;
  return true;
}

const char* compassWord(float az) {
  static const char* const kWords[] = {"north", "northeast", "east", "southeast",
                                       "south", "southwest", "west", "northwest"};
  int i = static_cast<int>(std::floor((az + 22.5f) / 45.0f)) % 8;
  if (i < 0) i += 8;
  return kWords[i];
}

void launchBearing(const Launch& l, float* bearing, float* dist) {
  orbit::bearingDistance(services::map_center::latitude(), services::map_center::longitude(),
                         l.pad_lat, l.pad_lon, bearing, dist);
}

void sourceStatus(char* out, size_t len) {
  const int64_t now = nowUnix();
  const long launch_age = s_launch_fetched_at > 0 && now > 0
                              ? static_cast<long>((now - s_launch_fetched_at) / 60) : -1;
  const long tle_age_h = s_tle_fetched_at > 0 && now > 0
                             ? static_cast<long>((now - s_tle_fetched_at) / 3600) : -1;
  char a[20];
  char b[20];
  if (launch_age >= 0) snprintf(a, sizeof(a), "%ld min ago", launch_age);
  else snprintf(a, sizeof(a), "pending");
  if (tle_age_h >= 0) snprintf(b, sizeof(b), "%ld h old", tle_age_h);
  else snprintf(b, sizeof(b), "pending");
  snprintf(out, len, "Launches %s, ISS orbit %s", a, b);
}

}  // namespace services::sky

#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C
