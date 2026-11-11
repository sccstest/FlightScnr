#include "ui/overhead_sign.h"

#include "config.h"

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)

#include <Arduino.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "data/airline_logos_lookup.h"
#include "data/airports_lookup.h"
#include "geo/flat_earth.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/aircraft_alert.h"
#include "services/aircraft_type_lookup.h"
#include "services/airline_logo.h"
#include "services/airport_lookup.h"
#include "services/map_center.h"
#include "services/route_info.h"
#include "services/route_lookup.h"
#include "ui/display_prefs.h"
#include "ui/radar_display.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"
#include "ui/ui_scale.h"

namespace ui {
namespace {

// --- Geometry (designed at 390 px, scaled) ----------------------------------

/** Clip radius for the backing: just inside the glass. */
constexpr int kClipRadius = radar::kCenterX - ui::sc(2);
/** Bottom of the second text row, measured up from the panel's bottom edge. */
constexpr int kLine2BottomInset = ui::sc(42);
constexpr int kLineGap = ui::sc(2);
constexpr int kTopPad = ui::sc(7);
/** Side margin between text and the bezel at the row's narrowest point. */
constexpr int kSideMargin = ui::sc(6);
constexpr int kLogoGap = ui::sc(6);
constexpr int kDotGap = ui::sc(6);   // space either side of a "·" separator
constexpr int kArrowW = ui::sc(16);  // drawn "→" body length
constexpr int kArrowGap = ui::sc(5);

/** Keep the current plane unless a rival is clearly closer (stops two planes at
 *  similar distance from flipping the sign every poll). */
constexpr float kSwitchRatio = 0.85f;
constexpr float kSwitchMinGainKm = 0.3f;

// --- Content -----------------------------------------------------------------

enum class ItemKind : uint8_t { Text, Dot, Arrow };

struct Item {
  ItemKind kind;
  char text[48];
};

constexpr int kMaxItems = 7;

struct Line {
  Item items[kMaxItems];
  int count = 0;
  bool accent_first = false;  ///< First text item drawn in the sign's amber.
};

struct SignState {
  bool visible = false;
  char hex[7] = {};
  char callsign[9] = {};
  char type_full[48] = {};
  char type_short[48] = {};
  char type_code[5] = {};
  char origin[5] = {};
  char dest[5] = {};
  char alt[20] = {};
  char dist[16] = {};
  char logo_icao[4] = {};
};

SignState s_state;

// Layout (computed once from font metrics).
bool s_layout_ready = false;
int s_l1_top = 0;
int s_l1_h = 0;
int s_l2_top = 0;
int s_l2_h = 0;
int s_band_top = 0;

// Scaled logo cache (PSRAM): RGB565 + 8-bit coverage, rebuilt only on ICAO change.
char s_logo_key[4] = {};
bool s_logo_ok = false;
int s_logo_w = 0;
int s_logo_h = 0;
uint16_t* s_logo_rgb = nullptr;
uint8_t* s_logo_a = nullptr;

void copyStr(char* dst, size_t len, const char* src) {
  if (len == 0) {
    return;
  }
  strncpy(dst, src != nullptr ? src : "", len - 1);
  dst[len - 1] = '\0';
}

void ensureLayout(PlaneGfx& gfx) {
  if (s_layout_ready) {
    return;
  }
  s_l1_h = displayFontHeight(gfx, displayFontBody());
  s_l2_h = displayFontHeight(gfx, displayFontTag());
  const int l2_bottom = radar::kSize - kLine2BottomInset;
  s_l2_top = l2_bottom - s_l2_h;
  s_l1_top = s_l2_top - kLineGap - s_l1_h;
  s_band_top = s_l1_top - kTopPad;
  s_layout_ready = true;
}

/** Half-width of the glass at row y (0 outside). */
int halfChordAt(int y, int radius) {
  const int dy = y - radar::kCenterY;
  const int r2 = radius * radius - dy * dy;
  if (r2 <= 0) {
    return 0;
  }
  return static_cast<int>(sqrtf(static_cast<float>(r2)));
}

/** Usable text width for a row spanning [top, top + h): the narrower edge wins. */
int rowMaxWidth(int top, int h) {
  const int a = halfChordAt(top, kClipRadius);
  const int b = halfChordAt(top + h, kClipRadius);
  const int half = a < b ? a : b;
  const int w = 2 * (half - kSideMargin);
  return w > 0 ? w : 0;
}

// --- Formatting ----------------------------------------------------------------

bool iataForIcao(const char* icao, char* iata_out) {
  if (icao == nullptr || strlen(icao) != 4) {
    return false;
  }
  for (size_t i = 0; i < data::airports::kIataCount; ++i) {
    data::airports::IataEntry entry;
    memcpy_P(&entry, &data::airports::kIataToIcao[i], sizeof(entry));
    if (entry.icao[0] == icao[0] && entry.icao[1] == icao[1] && entry.icao[2] == icao[2] &&
        entry.icao[3] == icao[3]) {
      memcpy(iata_out, entry.iata, 3);
      iata_out[3] = '\0';
      return iata_out[0] != '\0';
    }
  }
  return false;
}

/** "KTPA" -> "TPA" when a passenger code exists, else the ICAO code. */
void routeCode(const char* code, char* out, size_t out_len) {
  out[0] = '\0';
  if (code == nullptr || code[0] == '\0') {
    return;
  }
  char icao[5];
  if (services::airport::normalizeRouteCode(code, icao, sizeof(icao))) {
    char iata[4];
    copyStr(out, out_len, iataForIcao(icao, iata) ? iata : icao);
    return;
  }
  copyStr(out, out_len, code);
}

/** "Boeing 737-800" stays; "(long wing)" style notes are dropped. */
void friendlyType(const char* type_code, SignState* st) {
  st->type_full[0] = '\0';
  st->type_short[0] = '\0';
  copyStr(st->type_code, sizeof(st->type_code), type_code);
  if (type_code == nullptr || type_code[0] == '\0') {
    return;
  }
  char desc[64];
  if (!services::aircraft_type::lookupDescription(type_code, desc, sizeof(desc))) {
    return;
  }
  char* paren = strchr(desc, '(');
  if (paren != nullptr) {
    *paren = '\0';
  }
  size_t n = strlen(desc);
  while (n > 0 && desc[n - 1] == ' ') {
    desc[--n] = '\0';
  }
  copyStr(st->type_full, sizeof(st->type_full), desc);
  // Without the maker: "Airbus A-321neo" -> "A-321neo".
  const char* space = strchr(desc, ' ');
  if (space != nullptr && space[1] != '\0') {
    copyStr(st->type_short, sizeof(st->type_short), space + 1);
  }
}

/** "13000 ft" -> "13,000 ft" (or metres); "GND" stays. */
void formatAltitude(const char* alt_tag, char* out, size_t out_len) {
  out[0] = '\0';
  char raw[20];
  radar::formatAltitudeDisplay(alt_tag, raw, sizeof(raw));
  int v = 0;
  char unit[8] = {};
  if (sscanf(raw, "%d %7s", &v, unit) != 2) {
    copyStr(out, out_len, raw);
    return;
  }
  if (v >= 1000) {
    snprintf(out, out_len, "%d,%03d %s", v / 1000, v % 1000, unit);
  } else {
    snprintf(out, out_len, "%d %s", v, unit);
  }
}

void formatDistance(float km, char* out, size_t out_len) {
  float v = km;
  const char* unit = "km";
  switch (radar::distanceUnit()) {
    case radar::DistanceUnit::StatuteMile:
      v = km / radar::kStatuteMileKm;
      unit = "mi";
      break;
    case radar::DistanceUnit::NauticalMile:
      v = km / radar::kNauticalMileKm;
      unit = "nm";
      break;
    default:
      break;
  }
  if (v < 9.95f) {
    snprintf(out, out_len, "%.1f %s", static_cast<double>(v), unit);
  } else {
    snprintf(out, out_len, "%d %s", static_cast<int>(lroundf(v)), unit);
  }
}

void addText(Line* line, const char* text) {
  if (line->count >= kMaxItems || text == nullptr || text[0] == '\0') {
    return;
  }
  Item& it = line->items[line->count++];
  it.kind = ItemKind::Text;
  copyStr(it.text, sizeof(it.text), text);
}

void addMark(Line* line, ItemKind kind) {
  if (line->count >= kMaxItems) {
    return;
  }
  Item& it = line->items[line->count++];
  it.kind = kind;
  it.text[0] = '\0';
}

int lineWidth(PlaneGfx& gfx, const Line& line) {
  int w = 0;
  for (int i = 0; i < line.count; ++i) {
    const Item& it = line.items[i];
    switch (it.kind) {
      case ItemKind::Text:
        w += gfx.textWidth(it.text);
        break;
      case ItemKind::Dot:
        w += 2 * kDotGap + ui::sc(3);
        break;
      case ItemKind::Arrow:
        w += 2 * kArrowGap + kArrowW;
        break;
    }
  }
  return w;
}

// --- Picking -----------------------------------------------------------------

float distanceKm(const services::adsb::Aircraft& ac) {
  float e = 0.0f;
  float n = 0.0f;
  float d = 0.0f;
  geo::localOffsetKm(services::map_center::latitude(), services::map_center::longitude(),
                     ac.lat, ac.lon, &e, &n, &d);
  return d;
}

/** Closest aircraft, in-range ones first; sticky to the current pick. */
const services::adsb::Aircraft* pickAircraft(float* out_dist_km) {
  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  const bool hide_others = services::alert::hideNonAlertedEnabled();

  const services::adsb::Aircraft* best = nullptr;
  bool best_in = false;
  float best_d = 0.0f;
  const services::adsb::Aircraft* cur = nullptr;
  bool cur_in = false;
  float cur_d = 0.0f;

  for (size_t i = 0; i < n; ++i) {
    const services::adsb::Aircraft& ac = planes[i];
    if (hide_others && !services::alert::isHighlighted(ac)) {
      continue;
    }
    const bool in = radarDisplayIsInRange(ac);
    const float d = distanceKm(ac);
    if (best == nullptr || (in && !best_in) || (in == best_in && d < best_d)) {
      best = &ac;
      best_in = in;
      best_d = d;
    }
    if (s_state.visible && ac.hex[0] != '\0' && strcmp(ac.hex, s_state.hex) == 0) {
      cur = &ac;
      cur_in = in;
      cur_d = d;
    }
  }
  if (cur != nullptr && cur != best && cur_in == best_in &&
      (best_d > cur_d * kSwitchRatio || cur_d - best_d < kSwitchMinGainKm)) {
    best = cur;
    best_d = cur_d;
  }
  *out_dist_km = best_d;
  return best;
}

// --- Logo ----------------------------------------------------------------------

/** Box-filter the decoded logo into the cache, fitting a box_w x box_h box. */
bool buildLogo(const char* icao, int box_w, int box_h) {
  if (strncmp(s_logo_key, icao, 3) == 0 && s_logo_key[0] != '\0') {
    return s_logo_ok;
  }
  copyStr(s_logo_key, sizeof(s_logo_key), icao);
  s_logo_ok = false;

  const uint16_t* src = nullptr;
  int sw = 0;
  int sh = 0;
  if (!services::airline::decodeLogoPixels(icao, &src, &sw, &sh) || sw <= 0 || sh <= 0) {
    return false;
  }
  if (s_logo_rgb == nullptr) {
    constexpr size_t kPx = data::airline_logos::kMaxLogoW * data::airline_logos::kMaxLogoH;
    s_logo_rgb = static_cast<uint16_t*>(ps_malloc(kPx * sizeof(uint16_t)));
    s_logo_a = static_cast<uint8_t*>(ps_malloc(kPx));
    if (s_logo_rgb == nullptr || s_logo_a == nullptr) {
      return false;
    }
  }
  float scale = 1.0f;
  if (sh > box_h) {
    scale = static_cast<float>(box_h) / static_cast<float>(sh);
  }
  if (sw * scale > box_w) {
    scale = static_cast<float>(box_w) / static_cast<float>(sw);
  }
  const int dw = static_cast<int>(lroundf(sw * scale)) > 0 ? static_cast<int>(lroundf(sw * scale)) : 1;
  const int dh = static_cast<int>(lroundf(sh * scale)) > 0 ? static_cast<int>(lroundf(sh * scale)) : 1;
  const uint16_t key = data::airline_logos::kTransparentRgb565;

  for (int dy = 0; dy < dh; ++dy) {
    const int y0 = dy * sh / dh;
    int y1 = (dy + 1) * sh / dh;
    if (y1 <= y0) {
      y1 = y0 + 1;
    }
    for (int dx = 0; dx < dw; ++dx) {
      const int x0 = dx * sw / dw;
      int x1 = (dx + 1) * sw / dw;
      if (x1 <= x0) {
        x1 = x0 + 1;
      }
      uint32_t r = 0, g = 0, b = 0, opaque = 0, total = 0;
      for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
          const uint16_t p = src[y * sw + x];
          ++total;
          if (p == key) {
            continue;
          }
          ++opaque;
          r += (p >> 11) & 0x1F;
          g += (p >> 5) & 0x3F;
          b += p & 0x1F;
        }
      }
      const size_t o = static_cast<size_t>(dy) * dw + dx;
      if (opaque == 0) {
        s_logo_rgb[o] = 0;
        s_logo_a[o] = 0;
      } else {
        s_logo_rgb[o] = static_cast<uint16_t>(((r / opaque) << 11) | ((g / opaque) << 5) |
                                              (b / opaque));
        s_logo_a[o] = static_cast<uint8_t>((opaque * 255U) / total);
      }
    }
  }
  s_logo_w = dw;
  s_logo_h = dh;
  s_logo_ok = true;
  return true;
}

inline uint16_t blend565(uint16_t dst, uint16_t src, uint32_t a) {
  const uint32_t ia = 255U - a;
  const uint32_t r = (((dst >> 11) & 0x1F) * ia + ((src >> 11) & 0x1F) * a) / 255U;
  const uint32_t g = (((dst >> 5) & 0x3F) * ia + ((src >> 5) & 0x3F) * a) / 255U;
  const uint32_t b = ((dst & 0x1F) * ia + (src & 0x1F) * a) / 255U;
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

void paintLogo(uint16_t* fb, int fb_w, int fb_h, int x, int y) {
  for (int j = 0; j < s_logo_h; ++j) {
    const int py = y + j;
    if (py < 0 || py >= fb_h) {
      continue;
    }
    for (int i = 0; i < s_logo_w; ++i) {
      const int px = x + i;
      if (px < 0 || px >= fb_w) {
        continue;
      }
      const size_t o = static_cast<size_t>(j) * s_logo_w + i;
      const uint8_t a = s_logo_a[o];
      if (a == 0) {
        continue;
      }
      uint16_t& d = fb[static_cast<size_t>(py) * fb_w + px];
      d = a >= 250 ? s_logo_rgb[o] : blend565(d, s_logo_rgb[o], a);
    }
  }
}

// --- Drawing -------------------------------------------------------------------

/** Darken the circular cap below band_top (semi-opaque black backing). */
void paintBacking(PlaneGfx& gfx, uint16_t* fb, int fb_w, int fb_h) {
  const int bottom = radar::kCenterY + kClipRadius;
  for (int y = s_band_top; y <= bottom && y < fb_h; ++y) {
    const int half = halfChordAt(y, kClipRadius);
    if (half <= 0) {
      continue;
    }
    const int x0 = radar::kCenterX - half;
    const int x1 = radar::kCenterX + half;
    if (fb == nullptr) {
      gfx.fillRect(static_cast<int16_t>(x0), static_cast<int16_t>(y),
                   static_cast<int16_t>(x1 - x0), 1, gfx.color565(12, 12, 14));
      continue;
    }
    uint16_t* row = fb + static_cast<size_t>(y) * fb_w;
    for (int x = x0 < 0 ? 0 : x0; x < x1 && x < fb_w; ++x) {
      const uint16_t p = row[x];
      // ~88% black: keep 30/256 of each channel. The map still shows through, but
      // plane tags underneath stay too faint to fight the sign's text.
      const uint16_t r = static_cast<uint16_t>((((p >> 11) & 0x1F) * 30) >> 8);
      const uint16_t g = static_cast<uint16_t>((((p >> 5) & 0x3F) * 30) >> 8);
      const uint16_t b = static_cast<uint16_t>(((p & 0x1F) * 30) >> 8);
      row[x] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
    }
  }
  // Thin amber rule along the top edge, like the lit frame of an LED sign.
  const int half = halfChordAt(s_band_top, kClipRadius);
  if (half > 0) {
    gfx.fillRect(static_cast<int16_t>(radar::kCenterX - half), static_cast<int16_t>(s_band_top),
                 static_cast<int16_t>(2 * half), static_cast<int16_t>(ui::sc(1) > 0 ? ui::sc(1) : 1),
                 gfx.color565(200, 140, 30));
  }
}

void drawLine(PlaneGfx& gfx, const Line& line, int x, int top, int h, uint16_t accent,
              uint16_t ink, uint16_t dim) {
  const int mid = top + h / 2;
  gfx.setTextDatum(TextDatum::MiddleLeft);
  bool first_text = true;
  for (int i = 0; i < line.count; ++i) {
    const Item& it = line.items[i];
    switch (it.kind) {
      case ItemKind::Text:
        gfx.setTextColor(first_text && line.accent_first ? accent : ink);
        gfx.drawString(it.text, static_cast<int16_t>(x), static_cast<int16_t>(mid));
        x += gfx.textWidth(it.text);
        first_text = false;
        break;
      case ItemKind::Dot: {
        const int r = ui::sc(3) / 2 > 1 ? ui::sc(3) / 2 : 1;
        gfx.fillCircle(static_cast<int16_t>(x + kDotGap + r), static_cast<int16_t>(mid),
                       static_cast<int16_t>(r), dim);
        x += 2 * kDotGap + ui::sc(3);
        break;
      }
      case ItemKind::Arrow: {
        const int x0 = x + kArrowGap;
        const int x1 = x0 + kArrowW;
        const int head = ui::sc(5);
        gfx.drawWideLine(static_cast<int16_t>(x0), static_cast<int16_t>(mid),
                         static_cast<int16_t>(x1 - head / 2), static_cast<int16_t>(mid),
                         ui::scf(1.2f), dim);
        gfx.fillTriangle(static_cast<int16_t>(x1), static_cast<int16_t>(mid),
                         static_cast<int16_t>(x1 - head), static_cast<int16_t>(mid - head),
                         static_cast<int16_t>(x1 - head), static_cast<int16_t>(mid + head), dim);
        x += 2 * kArrowGap + kArrowW;
        break;
      }
    }
  }
}

/** Line 1: callsign · type, trying type names [first, last] longest first.
 *  Returns false (with the shortest attempt in *out) when none fits. */
bool buildLine1(PlaneGfx& gfx, int max_w, int first, int last, Line* out) {
  const char* ident = s_state.callsign[0] != '\0' ? s_state.callsign : s_state.hex;
  const char* candidates[] = {s_state.type_full, s_state.type_short, s_state.type_code, ""};
  displayFontApply(gfx, displayFontBody());
  bool any = false;
  for (int i = first; i <= last; ++i) {
    const char* type = candidates[i];
    if (type[0] == '\0' && i != 3) {
      continue;
    }
    Line l;
    l.accent_first = true;
    addText(&l, ident);
    if (type[0] != '\0') {
      addMark(&l, ItemKind::Dot);
      addText(&l, type);
    }
    *out = l;
    any = true;
    if (lineWidth(gfx, l) <= max_w) {
      return true;
    }
  }
  if (!any) {
    Line l;
    l.accent_first = true;
    addText(&l, ident);
    *out = l;
  }
  return false;
}

/** Line 2: route · altitude · distance, dropping the least useful part to fit. */
void buildLine2(PlaneGfx& gfx, int max_w, Line* out) {
  displayFontApply(gfx, displayFontTag());
  const bool has_route = s_state.origin[0] != '\0' && s_state.dest[0] != '\0';
  // Variants in preference order: bit0 route, bit1 altitude, bit2 distance.
  const uint8_t variants[] = {0x7, 0x3, 0x5, 0x6, 0x1, 0x2, 0x4};
  for (uint8_t v : variants) {
    if ((v & 0x1) && !has_route) {
      continue;
    }
    Line l;
    if (v & 0x1) {
      addText(&l, s_state.origin);
      addMark(&l, ItemKind::Arrow);
      addText(&l, s_state.dest);
    }
    if ((v & 0x2) && s_state.alt[0] != '\0') {
      if (l.count > 0) {
        addMark(&l, ItemKind::Dot);
      }
      addText(&l, s_state.alt);
    }
    if (v & 0x4) {
      if (l.count > 0) {
        addMark(&l, ItemKind::Dot);
      }
      addText(&l, s_state.dist);
    }
    *out = l;
    if (lineWidth(gfx, l) <= max_w) {
      return;
    }
  }
}

}  // namespace

bool overheadSignUpdate() {
  SignState next;
  if (displayPrefsOverheadSignEnabled()) {
    float d = 0.0f;
    const services::adsb::Aircraft* ac = pickAircraft(&d);
    if (ac != nullptr) {
      next.visible = true;
      copyStr(next.hex, sizeof(next.hex), ac->hex);
      copyStr(next.callsign, sizeof(next.callsign), ac->callsign);
      friendlyType(ac->type, &next);
      // Route/airline: the plane's own fields, else what the detail screen already
      // cached in RAM this session (radar polls don't run route enrichment).
      const char* origin = ac->route_origin;
      const char* dest = ac->route_dest;
      const char* airline_icao = ac->airline_icao;
      services::route::RouteInfo cached;
      if ((origin[0] == '\0' || dest[0] == '\0') &&
          services::route::peekRamRoute(ac->callsign, &cached)) {
        origin = cached.origin;
        dest = cached.dest;
        if (airline_icao[0] == '\0') {
          airline_icao = cached.airline_icao;
        }
      }
      routeCode(origin, next.origin, sizeof(next.origin));
      routeCode(dest, next.dest, sizeof(next.dest));
      formatAltitude(ac->alt, next.alt, sizeof(next.alt));
      formatDistance(d, next.dist, sizeof(next.dist));
      if (strnlen(airline_icao, 4) == 3) {
        copyStr(next.logo_icao, sizeof(next.logo_icao), airline_icao);
      } else if (isalpha(static_cast<unsigned char>(ac->callsign[0])) &&
                 isalpha(static_cast<unsigned char>(ac->callsign[1])) &&
                 isalpha(static_cast<unsigned char>(ac->callsign[2])) &&
                 isdigit(static_cast<unsigned char>(ac->callsign[3]))) {
        // Airline-style callsign ("AAL3088") before any route lookup: the operator
        // prefix is enough to find a built-in logo. No logo for it -> none drawn.
        memcpy(next.logo_icao, ac->callsign, 3);
        next.logo_icao[3] = '\0';
      }
    }
  }
  if (memcmp(&next, &s_state, sizeof(next)) == 0) {
    return false;
  }
  s_state = next;
  return true;
}

void overheadSignDraw(PlaneGfx& gfx, uint16_t* fb, int fb_w, int fb_h) {
  if (!s_state.visible) {
    return;
  }
  ensureLayout(gfx);
  paintBacking(gfx, fb, fb_w, fb_h);

  const uint16_t accent = gfx.color565(255, 186, 48);
  const uint16_t ink = gfx.color565(240, 240, 240);
  const uint16_t dim = gfx.color565(165, 165, 165);

  // Logo, inline before the callsign, at most one text row tall.
  bool logo = false;
  if (fb != nullptr && s_state.logo_icao[0] != '\0') {
    logo = buildLogo(s_state.logo_icao, 2 * s_l1_h, s_l1_h + ui::sc(2));
  }

  // Friendly type name beats the logo, the logo beats the bare ICAO code.
  const int l1_max = rowMaxWidth(s_l1_top, s_l1_h);
  Line l1;
  int logo_w = logo ? s_logo_w + kLogoGap : 0;
  bool fit = logo && buildLine1(gfx, l1_max - logo_w, 0, 1, &l1);
  if (!fit && buildLine1(gfx, l1_max, 0, 1, &l1)) {
    fit = true;
    logo = false;
  }
  if (!fit && logo) {
    fit = buildLine1(gfx, l1_max - logo_w, 2, 3, &l1);
  }
  if (!fit) {
    logo = false;
    buildLine1(gfx, l1_max, 2, 3, &l1);
  }
  if (!logo) {
    logo_w = 0;
  }
  displayFontApply(gfx, displayFontBody());
  const int l1_w = lineWidth(gfx, l1);
  const int l1_x = radar::kCenterX - (l1_w + logo_w) / 2;
  if (logo) {
    paintLogo(fb, fb_w, fb_h, l1_x, s_l1_top + (s_l1_h - s_logo_h) / 2);
  }
  drawLine(gfx, l1, l1_x + logo_w, s_l1_top, s_l1_h, accent, ink, dim);

  Line l2;
  buildLine2(gfx, rowMaxWidth(s_l2_top, s_l2_h), &l2);
  displayFontApply(gfx, displayFontTag());
  const int l2_w = lineWidth(gfx, l2);
  drawLine(gfx, l2, radar::kCenterX - l2_w / 2, s_l2_top, s_l2_h, accent, ink, dim);
  gfx.setTextDatum(TextDatum::TopLeft);
}

bool overheadSignBounds(int* x, int* y, int* w, int* h) {
  if (!s_layout_ready) {
    ensureLayout(tft);
  }
  const int half = halfChordAt(s_band_top, kClipRadius);
  *x = radar::kCenterX - half;
  *y = s_band_top;
  *w = 2 * half;
  *h = radar::kSize - s_band_top;
  return s_state.visible;
}

bool overheadSignHitTest(int16_t x, int16_t y, char* hex_out, size_t hex_len) {
  if (!s_state.visible || !s_layout_ready || s_state.hex[0] == '\0') {
    return false;
  }
  // A little slack above the band so a finger on the top rule still counts.
  if (y < s_band_top - ui::sc(6)) {
    return false;
  }
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  if (dx * dx + dy * dy > radar::kCenterX * radar::kCenterX) {
    return false;
  }
  copyStr(hex_out, hex_len, s_state.hex);
  return true;
}

}  // namespace ui

#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C
