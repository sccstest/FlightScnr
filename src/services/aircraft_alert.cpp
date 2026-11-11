#include "services/aircraft_alert.h"

#include <Arduino.h>
#include <Preferences.h>

#include "config.h"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "hardware/buzzer.h"
#include "hardware/pin_config.h"
#include "services/watch_reg_parse.h"
#include "services/watch_type_parse.h"
#include "geo/flat_earth.h"
#include "services/map_center.h"
#include "ui/aircraft_icon.h"
#include "ui/alert_flash.h"
#include "ui/radar_display.h"
#include "ui/radar_scale.h"
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
#include "services/notable_aircraft.h"
#endif

namespace services::alert {
namespace {

constexpr char kStoreNs[] = "flightscnr";
constexpr char kMilKey[] = "alert_mil";
constexpr char kEmrgKey[] = "alert_emrg";
constexpr char kHideKey[] = "alert_hide";
constexpr char kWatchKey[] = "alert_watch";
constexpr char kWatchTypeKey[] = "alert_wtype";
constexpr char kWatchRegKey[] = "alert_wreg";
constexpr char kHeliKey[] = "alert_heli";
constexpr char kPrivateKey[] = "alert_ga";
constexpr char kTypeColorsKey[] = "type_colors";
constexpr char kApproachKey[] = "alert_appr";
constexpr char kOverheadKey[] = "alert_ovh";
constexpr char kNotableKey[] = "alert_notbl";

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
constexpr bool kTypeColorsDefault = true;
#else
constexpr bool kTypeColorsDefault = false;  // S3 boards keep their original look
#endif

constexpr size_t kSeenCapacity = 32;
constexpr size_t kWatchMax = 16;
constexpr size_t kWatchCallsignLen = 9;
constexpr size_t kWatchBlobLen = 160;
constexpr unsigned long kPulseCycleMs = 250;

bool s_mil_enabled = config::kFactoryAlertMilitary;
bool s_emrg_enabled = config::kFactoryAlertEmergency;
bool s_hide_non_alerted = false;
bool s_heli_enabled = config::kFactoryAlertHelicopter;
bool s_private_enabled = config::kFactoryAlertPrivate;
bool s_type_colors = kTypeColorsDefault;
bool s_alert_on_approach = config::kFactoryAlertOnApproach;
bool s_overhead_any = config::kFactoryAlertOverheadAny;
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
bool s_notable_enabled = config::kFactoryAlertNotable;
#endif

char s_watch_blob[kWatchBlobLen] = "";
char s_watch_callsigns[kWatchMax][kWatchCallsignLen];
size_t s_watch_count = 0;

char s_watch_type_blob[kWatchTypeBlobLen] = "";
char s_watch_types[kWatchTypeMax][kWatchTypeLen];
size_t s_watch_type_count = 0;

char s_watch_reg_blob[kWatchRegBlobLen] = "";
char s_watch_regs[kWatchRegMax][kWatchRegLen];
size_t s_watch_reg_count = 0;

struct SeenEntry {
  uint32_t hash = 0;
  bool occupied = false;
};

SeenEntry s_seen[kSeenCapacity];
size_t s_seen_write = 0;
uint8_t s_seen_range_mi = 0;

bool classAlert(const services::adsb::Aircraft& ac);  // helicopter / private alerts
bool overheadAnyMatch(const services::adsb::Aircraft& ac);

uint32_t hashCallsign(const char* cs) {
  uint32_t h = 2166136261u;
  for (const char* p = cs; *p != '\0'; ++p) {
    h ^= static_cast<uint8_t>(*p);
    h *= 16777619u;
  }
  return h;
}

uint32_t hashAircraft(const services::adsb::Aircraft& ac) {
  // Prefer Mode S hex so empty/shared callsigns still get distinct alerts.
  uint32_t h = hashCallsign(ac.hex[0] != '\0' ? ac.hex : ac.callsign);
  if (ac.hex[0] != '\0' && ac.callsign[0] != '\0') {
    h ^= hashCallsign(ac.callsign);
  }
  return h;
}

bool alreadySeen(uint32_t h) {
  for (size_t i = 0; i < kSeenCapacity; ++i) {
    if (s_seen[i].occupied && s_seen[i].hash == h) {
      return true;
    }
  }
  return false;
}

void markSeen(uint32_t h) {
  s_seen[s_seen_write].hash = h;
  s_seen[s_seen_write].occupied = true;
  s_seen_write = (s_seen_write + 1) % kSeenCapacity;
}

void clearSeen() {
  for (size_t i = 0; i < kSeenCapacity; ++i) {
    s_seen[i].occupied = false;
    s_seen[i].hash = 0;
  }
  s_seen_write = 0;
}

bool shouldAlert(const services::adsb::Aircraft& ac);

/** Drop hashes for aircraft that left the ring or the ADS-B list. */
void pruneSeenOutsideRange(const services::adsb::Aircraft* list, size_t count) {
  for (size_t i = 0; i < kSeenCapacity; ++i) {
    if (!s_seen[i].occupied) {
      continue;
    }
    bool still_in_range = false;
    for (size_t j = 0; j < count; ++j) {
      if (!shouldAlert(list[j]) || !ui::radarDisplayIsInRange(list[j])) {
        continue;
      }
      if (hashAircraft(list[j]) == s_seen[i].hash) {
        still_in_range = true;
        break;
      }
    }
    if (!still_in_range) {
      s_seen[i].occupied = false;
      s_seen[i].hash = 0;
    }
  }
}

bool isValidWatchCallsign(const char* cs) {
  if (cs == nullptr || cs[0] == '\0') {
    return false;
  }
  size_t len = strlen(cs);
  if (len < 4 || len >= kWatchCallsignLen) {
    return false;
  }
  for (int i = 0; i < 3; ++i) {
    if (!isalpha(static_cast<unsigned char>(cs[i]))) {
      return false;
    }
  }
  for (size_t i = 3; cs[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(cs[i]);
    if (!isupper(c) && !isdigit(c)) {
      return false;
    }
  }
  return true;
}

void normalizeWatchCallsign(const char* in, char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  char scratch[kWatchCallsignLen];
  const char* src = in;
  if (in == nullptr || in[0] == '\0') {
    out[0] = '\0';
    return;
  }
  if (in == out) {
    strncpy(scratch, in, sizeof(scratch) - 1);
    scratch[sizeof(scratch) - 1] = '\0';
    src = scratch;
  }
  out[0] = '\0';
  if (out_len < 2) {
    return;
  }
  size_t n = 0;
  for (size_t i = 0; src[i] != '\0' && n + 1 < out_len && n < kWatchCallsignLen - 1; ++i) {
    const unsigned char c = static_cast<unsigned char>(src[i]);
    if (isspace(c)) {
      continue;
    }
    out[n++] = static_cast<char>(toupper(c));
  }
  out[n] = '\0';
}

bool watchlistContains(const char* callsign) {
  if (callsign == nullptr || callsign[0] == '\0' || s_watch_count == 0) {
    return false;
  }
  char normalized[kWatchCallsignLen];
  normalizeWatchCallsign(callsign, normalized, sizeof(normalized));
  if (!isValidWatchCallsign(normalized)) {
    return false;
  }
  for (size_t i = 0; i < s_watch_count; ++i) {
    if (strcmp(s_watch_callsigns[i], normalized) == 0) {
      return true;
    }
  }
  return false;
}

bool watchTypeContains(const char* type) {
  return watchTypeListContains(s_watch_types, s_watch_type_count, type);
}

bool watchRegContains(const char* registration) {
  return watchRegListContains(s_watch_regs, s_watch_reg_count, registration);
}

void parseWatchBlob(const char* blob) {
  s_watch_count = 0;
  if (blob == nullptr || blob[0] == '\0') {
    return;
  }
  char token[kWatchCallsignLen];
  const char* start = blob;
  while (*start != '\0' && s_watch_count < kWatchMax) {
    while (*start == ',' || isspace(static_cast<unsigned char>(*start))) {
      ++start;
    }
    if (*start == '\0') {
      break;
    }
    const char* end = start;
    while (*end != '\0' && *end != ',') {
      ++end;
    }
    const size_t len = static_cast<size_t>(end - start);
    if (len == 0) {
      start = end;
      continue;
    }
    size_t copy_len = len;
    if (copy_len >= sizeof(token)) {
      copy_len = sizeof(token) - 1;
    }
    memcpy(token, start, copy_len);
    token[copy_len] = '\0';
    char normalized[kWatchCallsignLen];
    normalizeWatchCallsign(token, normalized, sizeof(normalized));
    if (!isValidWatchCallsign(normalized)) {
      Serial.printf("[alert] watch reject '%s'\n", normalized[0] != '\0' ? normalized : token);
      start = (*end == ',') ? end + 1 : end;
      continue;
    }
    bool dup = false;
    for (size_t i = 0; i < s_watch_count; ++i) {
      if (strcmp(s_watch_callsigns[i], normalized) == 0) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      strncpy(s_watch_callsigns[s_watch_count], normalized, kWatchCallsignLen - 1);
      s_watch_callsigns[s_watch_count][kWatchCallsignLen - 1] = '\0';
      ++s_watch_count;
    }
    start = (*end == ',') ? end + 1 : end;
  }
}

void rebuildWatchBlob() {
  s_watch_blob[0] = '\0';
  size_t used = 0;
  for (size_t i = 0; i < s_watch_count; ++i) {
    if (i > 0) {
      if (used + 1 < sizeof(s_watch_blob)) {
        s_watch_blob[used++] = ',';
        s_watch_blob[used] = '\0';
      }
    }
    const size_t n = strlen(s_watch_callsigns[i]);
    if (used + n >= sizeof(s_watch_blob)) {
      break;
    }
    memcpy(s_watch_blob + used, s_watch_callsigns[i], n);
    used += n;
    s_watch_blob[used] = '\0';
  }
}

void applyWatchTypeBlob(const char* blob) {
  s_watch_type_count = parseWatchTypeBlob(blob, s_watch_types, kWatchTypeMax);
  rebuildWatchTypeBlob(s_watch_types, s_watch_type_count, s_watch_type_blob,
                       sizeof(s_watch_type_blob));
}

void applyWatchRegBlob(const char* blob) {
  s_watch_reg_count = parseWatchRegBlob(blob, s_watch_regs, kWatchRegMax);
  rebuildWatchRegBlob(s_watch_regs, s_watch_reg_count, s_watch_reg_blob,
                      sizeof(s_watch_reg_blob));
}

bool isAlertTarget(const services::adsb::Aircraft& ac);

bool shouldAlert(const services::adsb::Aircraft& ac) {
  if (s_emrg_enabled && ac.isEmergencySquawk()) {
    return true;  // emergencies never wait for an approach
  }
  if (isAlertTarget(ac) && (!s_alert_on_approach || approachingOverhead(ac))) {
    return true;
  }
  return overheadAnyMatch(ac);
}

bool isAlertTarget(const services::adsb::Aircraft& ac) {
  if (s_mil_enabled && ac.isMilitary()) {
    return true;
  }
  if (classAlert(ac)) {
    return true;
  }
  if (s_emrg_enabled && ac.isEmergencySquawk()) {
    return true;
  }
  if (watchlistContains(ac.callsign)) {
    return true;
  }
  if (watchTypeContains(ac.type)) {
    return true;
  }
  if (watchRegContains(ac.registration)) {
    return true;
  }
  return false;
}

bool alertsActive() {
  return s_mil_enabled || s_emrg_enabled || s_heli_enabled || s_private_enabled ||
         s_overhead_any ||
         s_watch_count > 0 || s_watch_type_count > 0 || s_watch_reg_count > 0;
}

/** Airline flight numbers: 3-letter ICAO operator code then a digit (AAL576, UPS1340). */
bool looksLikeAirlineFlight(const char* cs) {
  return cs != nullptr && isalpha(static_cast<unsigned char>(cs[0])) &&
         isalpha(static_cast<unsigned char>(cs[1])) &&
         isalpha(static_cast<unsigned char>(cs[2])) &&
         isdigit(static_cast<unsigned char>(cs[3]));
}

bool classAlert(const services::adsb::Aircraft& ac) {
  if (!s_heli_enabled && !s_private_enabled) {
    return false;
  }
  const TrafficClass c = trafficClass(ac);
  return (s_heli_enabled && c == TrafficClass::Helicopter) ||
         (s_private_enabled && c == TrafficClass::Private);
}

int altitudeFeet(const services::adsb::Aircraft& ac, bool* on_ground) {
  *on_ground = false;
  if (ac.alt[0] == 'g' || ac.alt[0] == 'G') {
    *on_ground = true;
    return 0;
  }
  if (ac.alt[0] == '\0') {
    return -1;
  }
  return atoi(ac.alt);
}

bool overheadAnyMatch(const services::adsb::Aircraft& ac) {
  if (!s_overhead_any) {
    return false;
  }
  bool ground = false;
  const int ft = altitudeFeet(ac, &ground);
  if (ground || ft < 0 || ft > kOverheadMaxAltFt) {
    return false;
  }
  return approachingOverhead(ac);
}

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
// --- "Go outside and look" notable-aircraft alarm --------------------------------
// One entry per notable airframe seen recently; fired once per pass. A pass ends when
// the aircraft has been missing from the feed for kNotablePassGapMs.
constexpr size_t kNotableCapacity = 16;
constexpr unsigned long kNotablePassGapMs = 3UL * 60UL * 1000UL;

struct NotablePass {
  uint32_t hash = 0;
  unsigned long last_ms = 0;
  bool used = false;
  bool fired = false;
};

NotablePass s_notable[kNotableCapacity];

/** Hex-only identity, so a callsign appearing mid-pass does not start a new pass. */
uint32_t notableHash(const services::adsb::Aircraft& ac) {
  return hashCallsign(ac.hex[0] != '\0' ? ac.hex : ac.callsign);
}

NotablePass* notablePassFor(uint32_t h, unsigned long now) {
  NotablePass* oldest = &s_notable[0];
  for (NotablePass& p : s_notable) {
    if (p.used && p.hash == h) {
      return &p;
    }
  }
  for (NotablePass& p : s_notable) {
    if (!p.used) {
      oldest = &p;
      break;
    }
    if (static_cast<long>(oldest->last_ms - p.last_ms) > 0) {
      oldest = &p;
    }
  }
  *oldest = NotablePass{};
  oldest->used = true;
  oldest->hash = h;
  oldest->last_ms = now;
  return oldest;
}

/** True when this aircraft already had its notable alarm during the current pass. */
bool notableAnnounced(const services::adsb::Aircraft& ac) {
  if (!s_notable_enabled) {
    return false;
  }
  const uint32_t h = notableHash(ac);
  for (const NotablePass& p : s_notable) {
    if (p.used && p.fired && p.hash == h) {
      return true;
    }
  }
  return false;
}

/** Returns true when it fired the notable ring + chime this poll. */
bool checkNotable(const services::adsb::Aircraft* list, size_t count, bool* chime_played) {
  *chime_played = false;
  if (!s_notable_enabled) {
    return false;
  }
  const unsigned long now = millis();
  for (NotablePass& p : s_notable) {
    if (p.used && now - p.last_ms > kNotablePassGapMs) {
      p = NotablePass{};
    }
  }
  bool fired = false;
  for (size_t i = 0; i < count; ++i) {
    services::notable::Match m;
    if (!services::notable::match(list[i], &m)) {
      continue;
    }
    NotablePass* pass = notablePassFor(notableHash(list[i]), now);
    pass->last_ms = now;
    if (pass->fired) {
      continue;
    }
    // approachingOverhead() fills t/miss whenever the aircraft is closing, even when
    // it is outside the (smaller) overhead radius; t stays -1 when moving away.
    float t = -1.0f;
    float miss = 0.0f;
    approachingOverhead(list[i], &t, &miss);
    const bool approaching = t >= 0.0f && t <= kNotableLeadSec && miss <= kNotableRadiusMi;
    const bool appeared = m.rarity == services::notable::Rarity::VeryRare &&
                          ui::radarDisplayIsInRange(list[i]);
    if (!approaching && !appeared) {
      continue;
    }
    pass->fired = true;
    fired = true;
    if (approaching) {
      Serial.printf("[alert] NOTABLE %s (%s) type=%s approaching: closest %.1f mi in %.0f s\n",
                    m.label, list[i].callsign, list[i].type, static_cast<double>(miss),
                    static_cast<double>(t));
    } else {
      Serial.printf("[alert] NOTABLE %s (%s) type=%s appeared in range (very rare)\n",
                    m.label, list[i].callsign, list[i].type);
    }
  }
  if (fired) {
    ui::alertFlashStart(ui::AlertFlashKind::Notable);
#if !FLIGHTSCNR_HAS_HAPTIC
    hardware::buzzerAlert(hardware::chime::Kind::Notable);
    *chime_played = true;
#endif
  }
  return fired;
}
#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C

void persistBool(const char* key, bool value) {
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(key, value);
    prefs.end();
  }
}

}  // namespace

void bootLoad() {
  Preferences prefs;
  if (!prefs.begin(kStoreNs, true)) {
    Serial.println("[alert] NVS open failed");
    return;
  }
  s_mil_enabled = prefs.getBool(kMilKey, config::kFactoryAlertMilitary);
  s_emrg_enabled = prefs.getBool(kEmrgKey, config::kFactoryAlertEmergency);
  s_hide_non_alerted = prefs.getBool(kHideKey, false);
  s_heli_enabled = prefs.getBool(kHeliKey, config::kFactoryAlertHelicopter);
  s_private_enabled = prefs.getBool(kPrivateKey, config::kFactoryAlertPrivate);
  s_type_colors = prefs.getBool(kTypeColorsKey, kTypeColorsDefault);
  s_alert_on_approach = prefs.getBool(kApproachKey, config::kFactoryAlertOnApproach);
  s_overhead_any = prefs.getBool(kOverheadKey, config::kFactoryAlertOverheadAny);
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  s_notable_enabled = prefs.getBool(kNotableKey, config::kFactoryAlertNotable);
#endif
  const String watch = prefs.getString(kWatchKey, "");
  strncpy(s_watch_blob, watch.c_str(), sizeof(s_watch_blob) - 1);
  s_watch_blob[sizeof(s_watch_blob) - 1] = '\0';
  const String watch_types = prefs.getString(kWatchTypeKey, "");
  strncpy(s_watch_type_blob, watch_types.c_str(), sizeof(s_watch_type_blob) - 1);
  s_watch_type_blob[sizeof(s_watch_type_blob) - 1] = '\0';
  const String watch_regs = prefs.getString(kWatchRegKey, "");
  strncpy(s_watch_reg_blob, watch_regs.c_str(), sizeof(s_watch_reg_blob) - 1);
  s_watch_reg_blob[sizeof(s_watch_reg_blob) - 1] = '\0';
  prefs.end();
  parseWatchBlob(s_watch_blob);
  applyWatchTypeBlob(s_watch_type_blob);
  applyWatchRegBlob(s_watch_reg_blob);
  Serial.printf("[alert] boot mil=%d emrg=%d hide=%d watch=%u types=%u regs=%u\n",
                s_mil_enabled ? 1 : 0, s_emrg_enabled ? 1 : 0, s_hide_non_alerted ? 1 : 0,
                static_cast<unsigned>(s_watch_count),
                static_cast<unsigned>(s_watch_type_count),
                static_cast<unsigned>(s_watch_reg_count));
}

void checkNewAircraft(const services::adsb::Aircraft* list, size_t count) {
  // The notable alarm keeps its own once-per-pass memory, so it runs even when every
  // other alert is off and survives range changes.
  bool notable_chime = false;
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  checkNotable(list, count, &notable_chime);
#endif
  if (!alertsActive()) {
    return;
  }

  // Range change = new visible ring: forget prior entries so anyone inside the
  // new range (or who later flies into it) can alert again.
  const uint8_t range_mi = ui::radar::scaleActiveMiles();
  if (range_mi != s_seen_range_mi) {
    clearSeen();
    s_seen_range_mi = range_mi;
    Serial.printf("[alert] range→%umi (seen cleared)\n", static_cast<unsigned>(range_mi));
  } else {
    pruneSeenOutsideRange(list, count);
  }

  bool fired = false;
  bool fired_military = false;
  bool fired_heli = false;
  bool fired_private = false;
  bool fired_other = false;
  bool fired_overhead = false;
  bool fired_emergency = false;
  for (size_t i = 0; i < count; ++i) {
    if (!shouldAlert(list[i])) {
      continue;
    }
    // Rising edge only: inside the visible ring (full plane, not edge blip).
    if (!ui::radarDisplayIsInRange(list[i])) {
      continue;
    }
    const uint32_t h = hashAircraft(list[i]);
    if (alreadySeen(h)) {
      continue;
    }
    markSeen(h);
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    if (notableAnnounced(list[i]) && !(s_emrg_enabled && list[i].isEmergencySquawk())) {
      continue;  // already got its "go look" alarm this pass: one alarm per aircraft
    }
#endif
    fired = true;
    const TrafficClass cls = trafficClass(list[i]);
    if (!isAlertTarget(list[i]) && !(s_emrg_enabled && list[i].isEmergencySquawk())) {
      fired_overhead = true;  // only matched as "overhead (any)"
    } else if (s_mil_enabled && list[i].isMilitary()) {
      fired_military = true;
    } else if (s_heli_enabled && cls == TrafficClass::Helicopter) {
      fired_heli = true;
    } else if (s_private_enabled && cls == TrafficClass::Private) {
      fired_private = true;
    } else {
      fired_other = true;  // watch list / emergency
    }
    if (s_emrg_enabled && list[i].isEmergencySquawk()) {
      fired_emergency = true;  // the urgent chime wins over any other kind
    }
    {
      float t = 0.0f;
      float miss = 0.0f;
      if (approachingOverhead(list[i], &t, &miss)) {
        Serial.printf("[alert] %s approaching: closest %.1f mi in %.0f s\n", list[i].callsign,
                      static_cast<double>(miss), static_cast<double>(t));
      }
    }
    Serial.printf("[alert] %s type=%s reg=%s mil=%d emrg=%d watch=%d wtype=%d wreg=%d squawk=%s\n",
                  list[i].callsign, list[i].type, list[i].registration,
                  list[i].isMilitary() ? 1 : 0, list[i].isEmergencySquawk() ? 1 : 0,
                  watchlistContains(list[i].callsign) ? 1 : 0,
                  watchTypeContains(list[i].type) ? 1 : 0,
                  watchRegContains(list[i].registration) ? 1 : 0, list[i].squawk);
  }
  if (fired && notable_chime && !fired_emergency) {
    fired = false;  // the notable ring + chime already announced this poll
  }
  if (fired) {
    // Priority when several fire at once: military, watch/emergency, helicopter, private.
    const ui::AlertFlashKind kind = fired_military  ? ui::AlertFlashKind::Military
                                    : fired_other   ? ui::AlertFlashKind::Tracked
                                    : fired_heli    ? ui::AlertFlashKind::Helicopter
                                    : fired_private ? ui::AlertFlashKind::Private
                                    : fired_overhead ? ui::AlertFlashKind::Overhead
                                                     : ui::AlertFlashKind::Tracked;
    ui::alertFlashStart(kind);
#if !FLIGHTSCNR_HAS_HAPTIC
    // Speaker boards play the matching chime; piezo boards ignore the kind (triple beep).
    using hardware::chime::Kind;
    Kind chime = Kind::Emergency;
    if (!fired_emergency) {
      switch (kind) {
        case ui::AlertFlashKind::Military:   chime = Kind::Military; break;
        case ui::AlertFlashKind::Helicopter: chime = Kind::Helicopter; break;
        case ui::AlertFlashKind::Private:    chime = Kind::Private; break;
        case ui::AlertFlashKind::Overhead:   chime = Kind::Overhead; break;
        case ui::AlertFlashKind::Tracked:    chime = Kind::Tracked; break;
      }
    }
    hardware::buzzerAlert(chime);
#endif
  }
}

void notifyRangeChanged() {
  clearSeen();
  s_seen_range_mi = ui::radar::scaleActiveMiles();
  Serial.printf("[alert] range→%umi (seen cleared)\n",
                static_cast<unsigned>(s_seen_range_mi));
  checkNewAircraft(services::adsb::aircraftList(), services::adsb::aircraftCount());
}

void notifyRadarResumed() {
  clearSeen();
  Serial.println("[alert] radar resumed (seen cleared)");
  checkNewAircraft(services::adsb::aircraftList(), services::adsb::aircraftCount());
}

bool isHighlighted(const services::adsb::Aircraft& ac) {
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  if (s_notable_enabled && services::notable::match(ac, nullptr)) {
    return true;
  }
#endif
  if (overheadAnyMatch(ac)) {
    return true;
  }
  if (s_mil_enabled && ac.isMilitary()) {
    return true;
  }
  if (classAlert(ac)) {
    return true;
  }
  if (s_emrg_enabled && ac.isEmergencySquawk()) {
    return true;
  }
  if (watchlistContains(ac.callsign)) {
    return true;
  }
  if (watchTypeContains(ac.type)) {
    return true;
  }
  if (watchRegContains(ac.registration)) {
    return true;
  }
  return false;
}

bool pulsePhase() {
  return (millis() / kPulseCycleMs) % 2 == 0;
}

TrafficClass trafficClass(const services::adsb::Aircraft& ac) {
  if (ac.isMilitary()) {
    return TrafficClass::Military;
  }
  using services::adsb::AircraftCategory;
  const AircraftCategory cat = ac.classify();
  const uint8_t icon = ui::aircraft_icon::resolveCategory(ac);
  if (cat == AircraftCategory::Helicopter || ui::aircraft_icon::isHelicopterIcon(icon)) {
    return TrafficClass::Helicopter;
  }
  if (looksLikeAirlineFlight(ac.callsign)) {
    return TrafficClass::Airline;
  }
  if (cat == AircraftCategory::LightAircraft || cat == AircraftCategory::SmallAircraft ||
      cat == AircraftCategory::Glider || cat == AircraftCategory::LighterThanAir ||
      ui::aircraft_icon::isGeneralAviationIcon(icon)) {
    return TrafficClass::Private;
  }
  return TrafficClass::Airline;
}

bool approachingOverhead(const services::adsb::Aircraft& ac, float* t_sec, float* miss_mi) {
  float east = 0.0f;
  float north = 0.0f;
  float dist = 0.0f;
  geo::localOffsetKm(services::map_center::latitude(), services::map_center::longitude(),
                     ac.lat, ac.lon, &east, &north, &dist);
  constexpr float kKmPerMi = 1.609344f;
  const float radius_km = kOverheadRadiusMi * kKmPerMi;
  // Ground speed knots -> km/s along the ground track (0 deg = north).
  const float v = ac.gs_knots * 1.852f / 3600.0f;
  if (v < 0.005f) {  // ~10 kt: hovering / stationary
    if (t_sec != nullptr) *t_sec = 0.0f;
    if (miss_mi != nullptr) *miss_mi = dist / kKmPerMi;
    return dist <= radius_km;
  }
  const float rad = ac.track_deg * 0.01745329252f;
  const float vx = sinf(rad) * v;
  const float vy = cosf(rad) * v;
  const float t = -(east * vx + north * vy) / (v * v);  // seconds to closest approach
  if (t < 0.0f) {
    return false;  // moving away from home
  }
  const float cx = east + vx * t;
  const float cy = north + vy * t;
  const float miss = sqrtf(cx * cx + cy * cy);
  if (t_sec != nullptr) *t_sec = t;
  if (miss_mi != nullptr) *miss_mi = miss / kKmPerMi;
  return miss <= radius_km && t <= kOverheadLeadSec;
}

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
bool notableAlertEnabled() { return s_notable_enabled; }

void toggleNotableAlert() {
  s_notable_enabled = !s_notable_enabled;
  persistBool(kNotableKey, s_notable_enabled);
  for (NotablePass& p : s_notable) {
    p = NotablePass{};
  }
  ui::radarDisplayInvalidateAircraft();
}

void saveNotableFromForm(const char* checkbox) {
  const bool on = checkbox != nullptr && checkbox[0] == 'T';
  if (on != s_notable_enabled) {
    toggleNotableAlert();
  }
}

const char* notableTag(const services::adsb::Aircraft& ac) {
  services::notable::Match m;
  return s_notable_enabled && services::notable::match(ac, &m) ? m.tag : nullptr;
}

const char* notableLabel(const services::adsb::Aircraft& ac) {
  services::notable::Match m;
  return s_notable_enabled && services::notable::match(ac, &m) ? m.label : nullptr;
}
#endif

bool alertOnApproachEnabled() { return s_alert_on_approach; }
bool overheadAnyEnabled() { return s_overhead_any; }

void toggleAlertOnApproach() {
  s_alert_on_approach = !s_alert_on_approach;
  persistBool(kApproachKey, s_alert_on_approach);
  clearSeen();
}

void toggleOverheadAny() {
  s_overhead_any = !s_overhead_any;
  persistBool(kOverheadKey, s_overhead_any);
  clearSeen();
}

void saveApproachFromForm(const char* approach_checkbox, const char* overhead_checkbox) {
  s_alert_on_approach = approach_checkbox != nullptr && approach_checkbox[0] == 'T';
  s_overhead_any = overhead_checkbox != nullptr && overhead_checkbox[0] == 'T';
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kApproachKey, s_alert_on_approach);
    prefs.putBool(kOverheadKey, s_overhead_any);
    prefs.end();
  }
  clearSeen();
}

bool typeColorsEnabled() { return s_type_colors; }
bool helicopterAlertEnabled() { return s_heli_enabled; }
bool privateAlertEnabled() { return s_private_enabled; }

void toggleTypeColors() {
  s_type_colors = !s_type_colors;
  persistBool(kTypeColorsKey, s_type_colors);
}
void toggleMilitaryAlert() {
  s_mil_enabled = !s_mil_enabled;
  persistBool(kMilKey, s_mil_enabled);
  clearSeen();
}
void toggleEmergencyAlert() {
  s_emrg_enabled = !s_emrg_enabled;
  persistBool(kEmrgKey, s_emrg_enabled);
  clearSeen();
}
void toggleHelicopterAlert() {
  s_heli_enabled = !s_heli_enabled;
  persistBool(kHeliKey, s_heli_enabled);
  clearSeen();
}
void togglePrivateAlert() {
  s_private_enabled = !s_private_enabled;
  persistBool(kPrivateKey, s_private_enabled);
  clearSeen();
}

void saveClassAlertsFromForm(const char* heli_checkbox, const char* private_checkbox,
                             const char* type_colors_checkbox) {
  s_heli_enabled = heli_checkbox != nullptr && heli_checkbox[0] == 'T';
  s_private_enabled = private_checkbox != nullptr && private_checkbox[0] == 'T';
  s_type_colors = type_colors_checkbox != nullptr && type_colors_checkbox[0] == 'T';
  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kHeliKey, s_heli_enabled);
    prefs.putBool(kPrivateKey, s_private_enabled);
    prefs.putBool(kTypeColorsKey, s_type_colors);
    prefs.end();
  }
  clearSeen();
}

bool militaryAlertEnabled() { return s_mil_enabled; }
bool emergencyAlertEnabled() { return s_emrg_enabled; }
bool hideNonAlertedEnabled() { return s_hide_non_alerted; }
bool watchCallsignsEnabled() { return s_watch_count > 0; }
size_t watchCallsignCount() { return s_watch_count; }
bool watchTypesEnabled() { return s_watch_type_count > 0; }
size_t watchTypeCount() { return s_watch_type_count; }
bool watchRegsEnabled() { return s_watch_reg_count > 0; }
size_t watchRegCount() { return s_watch_reg_count; }

void watchCallsignsFormatted(char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  strncpy(out, s_watch_blob, out_len - 1);
  out[out_len - 1] = '\0';
}

void watchTypesFormatted(char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  strncpy(out, s_watch_type_blob, out_len - 1);
  out[out_len - 1] = '\0';
}

void watchRegsFormatted(char* out, size_t out_len) {
  if (out == nullptr || out_len == 0) {
    return;
  }
  strncpy(out, s_watch_reg_blob, out_len - 1);
  out[out_len - 1] = '\0';
}

void saveFromForm(const char* mil_checkbox, const char* emrg_checkbox,
                  const char* hide_checkbox, const char* watch_callsigns,
                  bool update_watch_callsigns, const char* watch_types,
                  bool update_watch_types, const char* watch_regs,
                  bool update_watch_regs) {
  const bool prev_mil = s_mil_enabled;
  const bool prev_emrg = s_emrg_enabled;
  const bool prev_hide = s_hide_non_alerted;
  const size_t prev_watch = s_watch_count;
  const size_t prev_types = s_watch_type_count;
  const size_t prev_regs = s_watch_reg_count;
  char prev_blob[sizeof(s_watch_blob)];
  char prev_type_blob[sizeof(s_watch_type_blob)];
  char prev_reg_blob[sizeof(s_watch_reg_blob)];
  strncpy(prev_blob, s_watch_blob, sizeof(prev_blob));
  prev_blob[sizeof(prev_blob) - 1] = '\0';
  strncpy(prev_type_blob, s_watch_type_blob, sizeof(prev_type_blob));
  prev_type_blob[sizeof(prev_type_blob) - 1] = '\0';
  strncpy(prev_reg_blob, s_watch_reg_blob, sizeof(prev_reg_blob));
  prev_reg_blob[sizeof(prev_reg_blob) - 1] = '\0';

  s_mil_enabled = (mil_checkbox != nullptr && mil_checkbox[0] == 'T');
  s_emrg_enabled = (emrg_checkbox != nullptr && emrg_checkbox[0] == 'T');
  s_hide_non_alerted = (hide_checkbox != nullptr && hide_checkbox[0] == 'T');

  if (update_watch_callsigns) {
    char scratch[kWatchBlobLen];
    scratch[0] = '\0';
    if (watch_callsigns != nullptr) {
      strncpy(scratch, watch_callsigns, sizeof(scratch) - 1);
      scratch[sizeof(scratch) - 1] = '\0';
    }
    parseWatchBlob(scratch);
    rebuildWatchBlob();
  }

  if (update_watch_types) {
    char scratch[kWatchTypeBlobLen];
    scratch[0] = '\0';
    if (watch_types != nullptr) {
      strncpy(scratch, watch_types, sizeof(scratch) - 1);
      scratch[sizeof(scratch) - 1] = '\0';
    }
    applyWatchTypeBlob(scratch);
  }

  if (update_watch_regs) {
    char scratch[kWatchRegBlobLen];
    scratch[0] = '\0';
    if (watch_regs != nullptr) {
      strncpy(scratch, watch_regs, sizeof(scratch) - 1);
      scratch[sizeof(scratch) - 1] = '\0';
    }
    applyWatchRegBlob(scratch);
  }

  Preferences prefs;
  if (prefs.begin(kStoreNs, false)) {
    prefs.putBool(kMilKey, s_mil_enabled);
    prefs.putBool(kEmrgKey, s_emrg_enabled);
    prefs.putBool(kHideKey, s_hide_non_alerted);
    prefs.putString(kWatchKey, s_watch_blob);
    prefs.putString(kWatchTypeKey, s_watch_type_blob);
    prefs.putString(kWatchRegKey, s_watch_reg_blob);
    prefs.end();
  }
  const bool watch_changed =
      s_watch_count != prev_watch || strcmp(s_watch_blob, prev_blob) != 0;
  const bool types_changed =
      s_watch_type_count != prev_types || strcmp(s_watch_type_blob, prev_type_blob) != 0;
  const bool regs_changed =
      s_watch_reg_count != prev_regs || strcmp(s_watch_reg_blob, prev_reg_blob) != 0;
  if (s_hide_non_alerted != prev_hide || s_mil_enabled != prev_mil ||
      s_emrg_enabled != prev_emrg || watch_changed || types_changed || regs_changed) {
    ui::radarDisplayInvalidateAircraft();
  }
  Serial.printf(
      "[alert] saved mil=%d emrg=%d hide=%d watch=%u (%s) types=%u (%s) regs=%u (%s)\n",
      s_mil_enabled ? 1 : 0, s_emrg_enabled ? 1 : 0, s_hide_non_alerted ? 1 : 0,
      static_cast<unsigned>(s_watch_count), s_watch_blob,
      static_cast<unsigned>(s_watch_type_count), s_watch_type_blob,
      static_cast<unsigned>(s_watch_reg_count), s_watch_reg_blob);
}

}  // namespace services::alert
