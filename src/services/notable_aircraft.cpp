#include "services/notable_aircraft.h"

// Only the 4C uses this (S3 flash is nearly full); the native unit tests build it too.
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C) || defined(PIO_UNIT_TESTING)

#include <cctype>
#include <cstring>

namespace services::notable {
namespace {

/**
 * One table row. Every non-null field must match.
 *  type     ICAO type codes, '|' separated ("B744|B748")
 *  callsign prefix pattern: letters exact, '#' = any digit; a trailing '$' means the
 *           callsign must end there ("AF1$"), otherwise more characters may follow
 *  reg      full registration pattern, '#' = any digit ("N4#RF")
 *  hex      exact Mode S address (lower or upper case)
 */
struct Rule {
  const char* type;
  const char* callsign;
  const char* reg;
  const char* hex;
  bool military_only;
  Rarity rarity;
  const char* label;
  const char* tag;
};

constexpr Rarity N = Rarity::Notable;
constexpr Rarity R = Rarity::VeryRare;

// Order is priority: specific operators first, then aircraft types.
constexpr Rule kRules[] = {
    // --- Presidential / VIP (US Air Force 89th Airlift Wing) ---
    {nullptr, "AF1$", nullptr, nullptr, false, R, "Air Force One", "Air Force One"},
    {nullptr, "AF2$", nullptr, nullptr, false, R, "Air Force Two", "Air Force Two"},
    {"VC25", nullptr, nullptr, nullptr, false, R, "Presidential VC-25", "VC-25"},
    {nullptr, nullptr, nullptr, "adfdf8", false, R, "Presidential VC-25", "VC-25"},
    {nullptr, nullptr, nullptr, "adfdf9", false, R, "Presidential VC-25", "VC-25"},
    {nullptr, "SAM#", nullptr, nullptr, false, R, "Special Air Mission", "SAM VIP"},

    // --- NOAA Hurricane Hunters (Lakeland, FL): N42RF/N43RF WP-3D, N49RF G-IV ---
    {"P3", nullptr, "N4#RF", nullptr, false, R, "NOAA Hurricane Hunter", "Hurricane Hunter"},
    {"P3", "NOAA#", nullptr, nullptr, false, R, "NOAA Hurricane Hunter", "Hurricane Hunter"},
    {"GLF4", nullptr, "N4#RF", nullptr, false, R, "NOAA Hurricane Hunter", "Hurricane Hunter"},
    {"GLF4", "NOAA#", nullptr, nullptr, false, R, "NOAA Hurricane Hunter", "Hurricane Hunter"},
    {nullptr, "NOAA#", nullptr, nullptr, false, N, "NOAA research aircraft", "NOAA"},

    // --- US Coast Guard (Air Station Clearwater: HC-130J, MH-60T). Callsigns are
    //     "C" + the 4-digit airframe number, or "CG" + digits. ---
    {"C30J|C130", "C####$", nullptr, nullptr, false, N, "Coast Guard HC-130J", "Coast Guard"},
    {"C30J|C130", "CG#", nullptr, nullptr, false, N, "Coast Guard HC-130J", "Coast Guard"},
    {"H60", "C####$", nullptr, nullptr, false, N, "Coast Guard MH-60T", "Coast Guard"},
    {"H60", "CG#", nullptr, nullptr, false, N, "Coast Guard MH-60T", "Coast Guard"},
    {"AS65|H65", nullptr, nullptr, nullptr, false, N, "Coast Guard MH-65", "Coast Guard"},
    {nullptr, "CG#", nullptr, nullptr, false, N, "Coast Guard aircraft", "Coast Guard"},
    {nullptr, "C####$", nullptr, nullptr, true, N, "Coast Guard aircraft", "Coast Guard"},

    // --- Military heavies and oddities ---
    {"K35R|K35E|KC35", nullptr, nullptr, nullptr, false, N, "KC-135 Stratotanker", "KC-135"},
    {"KC46", nullptr, nullptr, nullptr, false, N, "KC-46 Pegasus tanker", "KC-46"},
    {"KC10", nullptr, nullptr, nullptr, false, R, "KC-10 Extender tanker", "KC-10"},
    {"C17", nullptr, nullptr, nullptr, false, N, "C-17 Globemaster III", "C-17"},
    {"C5|C5M", nullptr, nullptr, nullptr, false, R, "C-5 Galaxy", "C-5 Galaxy"},
    {"C130|C30J", nullptr, nullptr, nullptr, false, N, "C-130 Hercules", "C-130"},
    {"A400", nullptr, nullptr, nullptr, false, R, "Airbus A400M Atlas", "A400M"},
    {"B52", nullptr, nullptr, nullptr, false, R, "B-52 Stratofortress", "B-52"},
    {"B1", nullptr, nullptr, nullptr, false, R, "B-1 Lancer bomber", "B-1"},
    {"B2", nullptr, nullptr, nullptr, false, R, "B-2 Spirit bomber", "B-2"},
    {"E3TF|E3CF", nullptr, nullptr, nullptr, false, R, "E-3 AWACS", "AWACS"},
    {"E6", nullptr, nullptr, nullptr, false, R, "E-6 Mercury", "E-6"},
    {"P8", nullptr, nullptr, nullptr, false, N, "P-8 Poseidon", "P-8"},
    {"V22", nullptr, nullptr, nullptr, false, N, "V-22 Osprey tiltrotor", "Osprey"},
    {"U2", nullptr, nullptr, nullptr, false, R, "U-2 Dragon Lady", "U-2"},
    {"F22", nullptr, nullptr, nullptr, false, R, "F-22 Raptor", "F-22"},
    {"F35", nullptr, nullptr, nullptr, false, N, "F-35 Lightning II", "F-35"},

    // --- Big or rare civil types ---
    {"BLCF", nullptr, nullptr, nullptr, false, R, "Boeing 747 Dreamlifter", "Dreamlifter"},
    {"A3ST|A337", nullptr, nullptr, nullptr, false, R, "Airbus Beluga", "Beluga"},
    {"SGUP", nullptr, nullptr, nullptr, false, R, "Super Guppy", "Super Guppy"},
    {"A388", nullptr, nullptr, nullptr, false, R, "Airbus A380 superjumbo", "A380"},
    {"A124", nullptr, nullptr, nullptr, false, R, "Antonov An-124 Ruslan", "An-124"},
    {"A225", nullptr, nullptr, nullptr, false, R, "Antonov An-225 Mriya", "An-225"},
    {"IL76", nullptr, nullptr, nullptr, false, R, "Ilyushin Il-76", "Il-76"},
    {"CONC", nullptr, nullptr, nullptr, false, R, "Concorde", "Concorde"},
    {"B741|B742|B743|B744|B748|B74S|B74R|B74D", nullptr, nullptr, nullptr, false, N,
     "Boeing 747 jumbo jet", "747 Jumbo"},
    {"B17", nullptr, nullptr, nullptr, false, R, "B-17 Flying Fortress", "B-17"},
    {"B29", nullptr, nullptr, nullptr, false, R, "B-29 Superfortress", "B-29"},
    {"DC3", nullptr, nullptr, nullptr, false, N, "Vintage Douglas DC-3", "DC-3"},
    {"P51", nullptr, nullptr, nullptr, false, N, "P-51 Mustang warbird", "P-51"},
};

constexpr size_t kRuleCount = sizeof(kRules) / sizeof(kRules[0]);

char up(char c) { return static_cast<char>(toupper(static_cast<unsigned char>(c))); }

bool empty(const char* s) { return s == nullptr || s[0] == '\0'; }

/** value equals one of the '|' separated alternatives (case-insensitive). */
bool typeMatches(const char* alts, const char* value) {
  if (empty(value)) {
    return false;
  }
  const char* a = alts;
  while (*a != '\0') {
    const char* v = value;
    while (*a != '\0' && *a != '|' && *v != '\0' && up(*a) == up(*v)) {
      ++a;
      ++v;
    }
    if ((*a == '\0' || *a == '|') && *v == '\0') {
      return true;
    }
    while (*a != '\0' && *a != '|') {
      ++a;
    }
    if (*a == '|') {
      ++a;
    }
  }
  return false;
}

/** '#' = digit, letters case-insensitive. full: value must end with the pattern. */
bool patternMatches(const char* pattern, const char* value, bool full) {
  if (empty(value)) {
    return false;
  }
  const char* p = pattern;
  const char* v = value;
  for (; *p != '\0' && *p != '$'; ++p, ++v) {
    if (*v == '\0') {
      return false;
    }
    if (*p == '#') {
      if (!isdigit(static_cast<unsigned char>(*v))) {
        return false;
      }
    } else if (up(*p) != up(*v)) {
      return false;
    }
  }
  return (full || *p == '$') ? *v == '\0' : true;
}

bool hexMatches(const char* want, const char* hex) {
  if (empty(hex)) {
    return false;
  }
  return patternMatches(want, hex, true);
}

bool ruleMatches(const Rule& r, const char* type, const char* callsign, const char* reg,
                 const char* hex, bool military) {
  if (r.military_only && !military) return false;
  if (r.type != nullptr && !typeMatches(r.type, type)) return false;
  if (r.callsign != nullptr && !patternMatches(r.callsign, callsign, false)) return false;
  if (r.reg != nullptr && !patternMatches(r.reg, reg, true)) return false;
  if (r.hex != nullptr && !hexMatches(r.hex, hex)) return false;
  return true;
}

Match toMatch(const Rule& r) {
  Match m;
  m.label = r.label;
  m.tag = r.tag;
  m.rarity = r.rarity;
  return m;
}

}  // namespace

bool match(const char* type, const char* callsign, const char* registration,
           const char* hex, bool military, Match* out) {
  for (size_t i = 0; i < kRuleCount; ++i) {
    if (ruleMatches(kRules[i], type, callsign, registration, hex, military)) {
      if (out != nullptr) {
        *out = toMatch(kRules[i]);
      }
      return true;
    }
  }
  return false;
}

size_t ruleCount() { return kRuleCount; }

Match ruleAt(size_t index) { return index < kRuleCount ? toMatch(kRules[index]) : Match{}; }

}  // namespace services::notable

#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C || PIO_UNIT_TESTING
