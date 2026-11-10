#pragma once
#include <stdint.h>
#include <stdio.h>

#include "Printable.h"
#include "WString.h"

class IPAddress : public Printable {
 public:
  IPAddress() = default;
  IPAddress(uint8_t a, uint8_t b, uint8_t c, uint8_t d) : b_{a, b, c, d} {}
  explicit IPAddress(uint32_t v) {
    b_[0] = v & 0xFF;
    b_[1] = (v >> 8) & 0xFF;
    b_[2] = (v >> 16) & 0xFF;
    b_[3] = (v >> 24) & 0xFF;
  }
  uint8_t operator[](int i) const { return b_[i & 3]; }
  uint8_t& operator[](int i) { return b_[i & 3]; }
  operator uint32_t() const {
    return b_[0] | (b_[1] << 8) | (b_[2] << 16) | (static_cast<uint32_t>(b_[3]) << 24);
  }
  bool operator==(const IPAddress& o) const {
    return static_cast<uint32_t>(*this) == static_cast<uint32_t>(o);
  }
  bool operator!=(const IPAddress& o) const { return !(*this == o); }
  String toString() const {
    char s[16];
    snprintf(s, sizeof(s), "%u.%u.%u.%u", b_[0], b_[1], b_[2], b_[3]);
    return String(s);
  }
  bool fromString(const char* s) {
    unsigned a, b, c, d;
    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    b_[0] = a; b_[1] = b; b_[2] = c; b_[3] = d;
    return true;
  }
  size_t printTo(Print& p) const override;

 private:
  uint8_t b_[4] = {0, 0, 0, 0};
};

extern const IPAddress INADDR_NONE;
