#pragma once

#include <Arduino.h>

namespace util {

/** True once millis() has reached deadline_ms. Wrap-safe (millis() rolls over every
 *  49.7 days) for deadlines less than ~24 days away; `millis() < deadline` is not. */
inline bool millisReached(unsigned long deadline_ms) {
  return static_cast<long>(millis() - deadline_ms) >= 0;
}

inline bool millisReached(unsigned long now_ms, unsigned long deadline_ms) {
  return static_cast<long>(now_ms - deadline_ms) >= 0;
}

}  // namespace util
