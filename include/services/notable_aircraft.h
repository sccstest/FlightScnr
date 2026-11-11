#pragma once

#include <cstddef>
#include <cstdint>

#include "services/adsb_client.h"

/**
 * "Go outside and look" aircraft: a small compiled-in table of unique or notable
 * traffic (NOAA Hurricane Hunters, Coast Guard, military heavies, Air Force One / SAM,
 * 747s, A380, Beluga, ...). Matching is by category only: ICAO type code, callsign
 * pattern, registration pattern, Mode S hex. Labels never name an owner or a person.
 *
 * Pure C++ (no Arduino), so the native unit tests can exercise the table.
 */
namespace services::notable {

enum class Rarity : uint8_t {
  Notable,   // alarm when it is about to pass near home
  VeryRare,  // also alarm as soon as it first shows up inside the radar ring
};

struct Match {
  const char* label = nullptr;  // friendly name for the detail screen (<= 22 chars, fits one line)
  const char* tag = nullptr;    // short name for the radar tag (<= 16 chars)
  Rarity rarity = Rarity::Notable;
};

constexpr size_t kMaxLabelLen = 22;
constexpr size_t kMaxTagLen = 16;

/**
 * Classify by raw fields (any may be nullptr / empty). military = readsb dbFlags bit0.
 * Returns true and fills out (if non-null) for the first table rule that matches.
 */
bool match(const char* type, const char* callsign, const char* registration,
           const char* hex, bool military, Match* out);

inline bool match(const services::adsb::Aircraft& ac, Match* out) {
  return match(ac.type, ac.callsign, ac.registration, ac.hex, ac.isMilitary(), out);
}

/** Table access for tests (label / tag length checks). */
size_t ruleCount();
Match ruleAt(size_t index);

}  // namespace services::notable
