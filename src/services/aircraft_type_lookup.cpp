#include "services/aircraft_type_lookup.h"

#include <Arduino.h>

#include <cstring>

#include "data/icao_types_lookup.h"

namespace services::aircraft_type {

namespace {

void normalizeIcaoType(const char* in, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  if (in == nullptr) {
    return;
  }
  size_t n = 0;
  while (in[n] != '\0' && n + 1 < out_len && n < 4) {
    const unsigned char c = static_cast<unsigned char>(in[n]);
    out[n] = static_cast<char>(isupper(c) ? c : toupper(c));
    ++n;
  }
  out[n] = '\0';
}

int compareProgmemCode(const char* a, const data::icao_types::Entry* entry) {
  char code[5];
  memcpy_P(code, entry->code, sizeof(code));
  return strcmp(a, code);
}

const data::icao_types::Entry* findEntry(const char* code) {
  if (code == nullptr || code[0] == '\0') {
    return nullptr;
  }

  size_t lo = 0;
  size_t hi = data::icao_types::kCount;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const data::icao_types::Entry* entry = &data::icao_types::kTypes[mid];
    const int cmp = compareProgmemCode(code, entry);
    if (cmp == 0) {
      return entry;
    }
    if (cmp < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return nullptr;
}

void readNameAt(uint16_t offset, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  out[0] = '\0';
  size_t i = 0;
  while (i + 1 < out_len) {
    const char c = static_cast<char>(pgm_read_byte(&data::icao_types::kNames[offset + i]));
    out[i] = c;
    if (c == '\0') {
      return;
    }
    ++i;
  }
  out[out_len - 1] = '\0';
}

}  // namespace

bool lookupDescription(const char* icao_type, char* out, size_t out_len) {
  if (out_len > 0) {
    out[0] = '\0';
  }
  char code[5];
  normalizeIcaoType(icao_type, code, sizeof(code));
  if (code[0] == '\0') {
    return false;
  }

  const data::icao_types::Entry* entry = findEntry(code);
  if (entry == nullptr) {
    return false;
  }

  uint16_t offset = 0;
  memcpy_P(&offset, &entry->name_offset, sizeof(offset));
  readNameAt(offset, out, out_len);
  return out[0] != '\0';
}

namespace {

constexpr size_t kMaxTok = 12;
constexpr size_t kTokLen = 40;

bool hasDigit(const char* s) {
  for (; *s != '\0'; ++s) {
    if (isdigit(static_cast<unsigned char>(*s))) return true;
  }
  return false;
}

bool isAllUpperAlpha(const char* s) {
  if (*s == '\0') return false;
  for (; *s != '\0'; ++s) {
    if (!isupper(static_cast<unsigned char>(*s))) return false;
  }
  return true;
}

/** "HAVILLAND" -> "Havilland", "douglas" -> "Douglas". */
void titleCase(char* s) {
  bool first = true;
  for (; *s != '\0'; ++s) {
    const unsigned char c = static_cast<unsigned char>(*s);
    *s = static_cast<char>(first ? toupper(c) : tolower(c));
    first = false;
  }
}

void copyStr(char* out, size_t out_len, const char* in) {
  if (out_len == 0) return;
  snprintf(out, out_len, "%s", in);
}

/** Join tok[from..n) with spaces. */
void joinTokens(char tok[][kTokLen], size_t from, size_t n, char* out, size_t out_len) {
  out[0] = '\0';
  size_t used = 0;
  for (size_t i = from; i < n; ++i) {
    const int w = snprintf(out + used, out_len - used, "%s%s", used > 0 ? " " : "", tok[i]);
    if (w < 0 || static_cast<size_t>(w) >= out_len - used) {
      out[out_len - 1] = '\0';
      return;
    }
    used += static_cast<size_t>(w);
  }
}

}  // namespace

bool formatMakeModel(const char* icao_type, char* make, size_t make_len, char* model,
                     size_t model_len) {
  if (make_len > 0) make[0] = '\0';
  if (model_len > 0) model[0] = '\0';
  char full[data::icao_types::kMaxNameLen + 1];
  if (!lookupDescription(icao_type, full, sizeof(full))) {
    return false;
  }

  // Drop parentheticals: "ERJ-170-200 (long wing)" -> "ERJ-170-200".
  char clean[sizeof(full)];
  size_t ci = 0;
  int depth = 0;
  for (size_t i = 0; full[i] != '\0' && ci + 1 < sizeof(clean); ++i) {
    if (full[i] == '(') { ++depth; continue; }
    if (full[i] == ')') { if (depth > 0) --depth; continue; }
    if (depth == 0) clean[ci++] = full[i];
  }
  clean[ci] = '\0';

  char tok[kMaxTok][kTokLen];
  size_t n = 0;
  for (char* p = strtok(clean, " "); p != nullptr && n < kMaxTok; p = strtok(nullptr, " ")) {
    copyStr(tok[n++], kTokLen, p);
  }
  if (n == 0) return false;

  // Make = first word, with a few data quirks fixed.
  char mk[kTokLen];
  size_t first_model = 1;
  copyStr(mk, sizeof(mk), tok[0]);
  char* dash = strchr(mk, '-');
  if (dash != nullptr && hasDigit(mk)) {
    // "Atr-72-600": maker and model fused in one word.
    char rest[kTokLen];
    copyStr(rest, sizeof(rest), dash + 1);
    *dash = '\0';
    for (char* c = mk; *c != '\0'; ++c) *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
    copyStr(tok[0], kTokLen, rest);
    first_model = 0;
  } else if (dash != nullptr) {
    // "Mcdonnell-douglas", "Bell-boeing": keep the first maker.
    *dash = '\0';
    titleCase(mk);
  }
  if (first_model == 1 && n > 1 && strlen(tok[1]) > 3 && isAllUpperAlpha(tok[1])) {
    // "Airbus HELICOPTERS", "De HAVILLAND", "General DYNAMICS".
    char second[kTokLen];
    copyStr(second, sizeof(second), tok[1]);
    titleCase(second);
    first_model = 2;
    if (strcmp(second, "Helicopters") != 0) {
      char both[kTokLen * 2];
      snprintf(both, sizeof(both), "%s %s", mk, second);
      if (strlen(both) <= kMakeMaxChars) {
        copyStr(mk, sizeof(mk), both);
      }
      // Too long for a tag ("Hawker Beechcraft", "General Dynamics"): keep the first
      // word. Initials ("HB") read as noise on the device.
    }
  }
  copyStr(make, make_len, mk);

  // Model = the rest, cut at the first '/', "A-321" -> "A321".
  size_t m0 = first_model;
  for (size_t i = m0; i < n; ++i) {
    char* slash = strchr(tok[i], '/');
    if (slash != nullptr) {
      *slash = '\0';
      n = i + 1;
      break;
    }
  }
  for (size_t i = m0; i < n; ++i) {
    char* t = tok[i];
    if (isalpha(static_cast<unsigned char>(t[0])) && t[1] == '-' &&
        isdigit(static_cast<unsigned char>(t[2]))) {
      memmove(t + 1, t + 2, strlen(t + 2) + 1);
    }
  }
  // "Hawker BEECHCRAFT Hawker 750": don't repeat the maker in the model.
  while (m0 + 1 < n && strcasecmp(tok[m0], mk) == 0) {
    ++m0;
  }
  char mdl[kTokLen * 2];
  joinTokens(tok, m0, n, mdl, sizeof(mdl));
  if (strlen(mdl) > kModelMaxChars) {
    // Skip filler words, then keep the longest tail of whole words that fits:
    // "BD-100 Challenger 350" -> "Challenger 350" is still too long -> see below.
    while (m0 < n && (strcmp(tok[m0], "Regional") == 0 || strcmp(tok[m0], "Jet") == 0 ||
                      strcmp(tok[m0], "Super") == 0)) {
      ++m0;
    }
    char best[kTokLen * 2] = "";
    for (size_t i = m0; i < n; ++i) {
      joinTokens(tok, i, n, best, sizeof(best));
      if (strlen(best) <= kModelMaxChars) break;
      best[0] = '\0';
    }
    bool numeric_only = best[0] != '\0';
    for (const char* c = best; *c != '\0'; ++c) {
      if (!isdigit(static_cast<unsigned char>(*c))) { numeric_only = false; break; }
    }
    if (best[0] == '\0' || numeric_only) {
      // Longest single word that fits, names before numbers ("Challenger").
      int pick = -1;
      for (size_t i = m0; i < n; ++i) {
        const size_t len = strlen(tok[i]);
        if (len > kModelMaxChars) continue;
        if (pick < 0) { pick = static_cast<int>(i); continue; }
        const bool a = isalpha(static_cast<unsigned char>(tok[i][0]));
        const bool b = isalpha(static_cast<unsigned char>(tok[pick][0]));
        if ((a && !b) || (a == b && len > strlen(tok[pick]))) pick = static_cast<int>(i);
      }
      if (pick >= 0) {
        copyStr(best, sizeof(best), tok[pick]);
      } else {
        copyStr(best, sizeof(best), mdl);
        best[kModelMaxChars] = '\0';
      }
    }
    copyStr(mdl, sizeof(mdl), best);
  }
  copyStr(model, model_len, mdl);
  return make[0] != '\0';
}

void formatRadarTagLabel(const char* icao_type, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  // Radar tags stay compact: ICAO type code only (e.g. B738), not the full name.
  normalizeIcaoType(icao_type, out, out_len);
}

}  // namespace services::aircraft_type
