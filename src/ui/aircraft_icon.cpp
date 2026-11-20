// Icon rasterizing is per-pixel float work; the framework default is -Os.
#pragma GCC optimize("O2")

#include "ui/aircraft_icon.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <vector>

#include "Arduino_TFT.h"
#include "util/big_static.h"

#include "hardware/pin_config.h"
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
// 720 px panel: 64 px masks so icons drawn at ~52 px are downsampled, not upscaled.
#include "data/aircraft_icons_lookup_hires.h"
#else
#include "data/aircraft_icons_lookup.h"
#endif
#include "hardware/plane_gfx.h"
#include "services/adsb_client.h"

namespace ui::aircraft_icon {
namespace {

constexpr float kDegToRad = 0.01745329252f;

void normalizeType(const char* in, char* out, size_t out_len) {
  if (out_len == 0) {
    return;
  }
  size_t o = 0;
  if (in != nullptr) {
    for (size_t i = 0; in[i] != '\0' && o + 1 < out_len; ++i) {
      const unsigned char c = static_cast<unsigned char>(in[i]);
      if (c == ' ' || c == '\t') {
        continue;
      }
      out[o++] = static_cast<char>(toupper(c));
    }
  }
  out[o] = '\0';
}

int typeCmpPgm(const char* needle, const char* pgm_type) {
  char buf[5];
  for (int i = 0; i < 4; ++i) {
    buf[i] = static_cast<char>(pgm_read_byte(&pgm_type[i]));
  }
  buf[4] = '\0';
  for (int i = 3; i >= 0; --i) {
    if (buf[i] == ' ' || buf[i] == '\0') {
      buf[i] = '\0';
    } else {
      break;
    }
  }
  return strcmp(needle, buf);
}

bool lookupExactType(const char* type, uint8_t* out_category) {
  if (type == nullptr || type[0] == '\0' || data::aircraft_icons::kTypeMapCount == 0) {
    return false;
  }
  size_t lo = 0;
  size_t hi = data::aircraft_icons::kTypeMapCount;
  while (lo < hi) {
    const size_t mid = lo + (hi - lo) / 2;
    const data::aircraft_icons::TypeEntry* entry = &data::aircraft_icons::kTypeMap[mid];
    const int cmp = typeCmpPgm(type, entry->type);
    if (cmp == 0) {
      *out_category = pgm_read_byte(&entry->category);
      return true;
    }
    if (cmp < 0) {
      hi = mid;
    } else {
      lo = mid + 1;
    }
  }
  return false;
}

bool lookupPrefixType(const char* type, uint8_t* out_category) {
  if (type == nullptr || type[0] == '\0') {
    return false;
  }
  const size_t n = strlen(type);
  for (size_t len = n; len >= 3; --len) {
    char prefix[5] = {};
    memcpy(prefix, type, len);
    prefix[len] = '\0';
    if (lookupExactType(prefix, out_category)) {
      return true;
    }
  }
  return false;
}

bool isHelicopterType(const char* type) {
  uint8_t cat = 0;
  if (lookupExactType(type, &cat) &&
      (cat == static_cast<uint8_t>(data::aircraft_icons::Category::helicopter) ||
       cat == static_cast<uint8_t>(data::aircraft_icons::Category::military_helicopter))) {
    return true;
  }
  static constexpr const char* kPrefixes[] = {"EC", "AS", "AW", "R4", "R6", "MI",
                                              "KA", "BK", "MD5", "UH", "AH", "CH",
                                              "MH", "OH", "TH", "NH"};
  for (const char* p : kPrefixes) {
    if (strncmp(type, p, strlen(p)) == 0) {
      return true;
    }
  }
  return false;
}

bool isFighterType(const char* type) {
  static constexpr const char* kPrefixes[] = {
      "F14", "F15", "F16", "F18", "F22", "F35", "F100", "F104", "F111", "F117",
      "EUFI", "RFAL", "HAWK", "TORN", "SU27", "SU30", "SU35", "MIG29", "MIG31",
      "JAS39", "M2K", "M346"};
  for (const char* p : kPrefixes) {
    if (strncmp(type, p, strlen(p)) == 0) {
      return true;
    }
  }
  return false;
}

uint8_t categoryFromAdsbEmitter(const char* cat) {
  if (cat == nullptr || cat[0] == '\0') {
    return data::aircraft_icons::kDefaultCategory;
  }
  using C = data::aircraft_icons::Category;
  if (strcmp(cat, "A7") == 0) {
    return static_cast<uint8_t>(C::helicopter);
  }
  if (strcmp(cat, "B6") == 0) {
    return static_cast<uint8_t>(C::drone);
  }
  if (strcmp(cat, "B1") == 0) {
    return static_cast<uint8_t>(C::glider);
  }
  if (strcmp(cat, "B2") == 0) {
    return static_cast<uint8_t>(C::balloon);
  }
  if (strcmp(cat, "A5") == 0) {
    return static_cast<uint8_t>(C::large_jet_2);
  }
  if (strcmp(cat, "A1") == 0 || strcmp(cat, "B4") == 0) {
    return static_cast<uint8_t>(C::small_prop_single);
  }
  if (strcmp(cat, "A2") == 0) {
    return static_cast<uint8_t>(C::small_prop_twin);
  }
  if (strcmp(cat, "A3") == 0 || strcmp(cat, "A4") == 0) {
    return static_cast<uint8_t>(C::medium_jet);
  }
  if (strcmp(cat, "A6") == 0) {
    return static_cast<uint8_t>(C::military_fighter);
  }
  if (strcmp(cat, "C1") == 0 || strcmp(cat, "C2") == 0 ||
      strcmp(cat, "c1") == 0 || strcmp(cat, "c2") == 0) {
    return static_cast<uint8_t>(C::ground_veh);
  }
  return data::aircraft_icons::kDefaultCategory;
}

uint8_t militaryFallback(const char* type) {
  using C = data::aircraft_icons::Category;
  if (isFighterType(type)) {
    return static_cast<uint8_t>(C::military_fighter);
  }
  return static_cast<uint8_t>(C::military_transport);
}

}  // namespace

uint8_t resolveCategory(const services::adsb::Aircraft& aircraft) {
  char type[5] = {};
  normalizeType(aircraft.type, type, sizeof(type));

  // Surface vehicles before the ICAO type map (trucks often have no `t` code).
  if (aircraft.classify() == services::adsb::AircraftCategory::GroundVehicle) {
    return static_cast<uint8_t>(data::aircraft_icons::Category::ground_veh);
  }
  const char c0 = static_cast<char>(toupper(static_cast<unsigned char>(aircraft.category[0])));
  const char c1 = aircraft.category[1];
  if (c0 == 'C' && (c1 == '1' || c1 == '2') &&
      (aircraft.category[2] == '\0' || aircraft.category[2] == ' ')) {
    return static_cast<uint8_t>(data::aircraft_icons::Category::ground_veh);
  }
  if (strcmp(aircraft.alt, "GND") == 0) {
    if (strcmp(type, "GRND") == 0 || strcmp(type, "VEH") == 0 ||
        strcmp(type, "GTRK") == 0 || strcmp(type, "FIRE") == 0 ||
        strcmp(type, "TOW") == 0 || strcmp(type, "RIV") == 0 ||
        strcmp(type, "BUS") == 0 || strcmp(type, "VAN") == 0 ||
        // Many airport trucks omit ICAO type entirely.
        type[0] == '\0') {
      return static_cast<uint8_t>(data::aircraft_icons::Category::ground_veh);
    }
  }

  uint8_t mapped = 0;
  const bool have_map = lookupExactType(type, &mapped) || lookupPrefixType(type, &mapped);
  if (have_map) {
    return mapped;
  }
  if (type[0] != '\0' && isHelicopterType(type)) {
    if (aircraft.isMilitary()) {
      return static_cast<uint8_t>(data::aircraft_icons::Category::military_helicopter);
    }
    return static_cast<uint8_t>(data::aircraft_icons::Category::helicopter);
  }
  if (aircraft.isMilitary()) {
    return militaryFallback(type);
  }
  return categoryFromAdsbEmitter(aircraft.category);
}

bool hasIcon(uint8_t category) {
  return category < data::aircraft_icons::kCategoryCount;
}

namespace {

// ---- Rotated-mask cache ----------------------------------------------------------
// Rotating a 64 px mask costs ~55x55 float samples per icon, and a radar rebuild draws
// 30-50 of them. The lit pixels depend only on (category, size, heading), so they are
// built once as horizontal runs and reused until evicted (LRU).

/** One horizontal run of lit pixels, relative to the icon center. */
struct Span {
  int8_t dy;
  int8_t x0;
  uint8_t len;
};

struct MaskEntry {
  bool used = false;
  uint8_t category = 0;
  uint8_t side = 0;
  uint8_t heading_step = 0;  // heading / kHeadingStepDeg
  uint32_t stamp = 0;
  Span* spans = nullptr;  // PSRAM
  uint16_t count = 0;
  uint16_t capacity = 0;
};

/** Headings are rounded to this many degrees: at most 1 degree off, which moves an
 *  icon tip by under half a pixel, and lets a plane hold its cached mask in turns. */
constexpr int kHeadingStepDeg = 2;
constexpr int kHeadingSteps = 360 / kHeadingStepDeg;
constexpr size_t kMaskCacheSize = 64;
// ~1.5 KB of entries plus ~0.5 KB of runs each: all in PSRAM on the 4C.
using MaskArray = MaskEntry[kMaskCacheSize];
FS_BIG_STATIC(MaskArray, s_masks)
#define s_masks FS_BIG_STATIC_REF(s_masks)
uint32_t s_mask_clock = 0;
std::vector<Span> s_build;  // scratch for one build (reserved big, so it lands in PSRAM)

struct SpanView {
  const Span* data;
  size_t count;
  const Span* begin() const { return data; }
  const Span* end() const { return data + count; }
};

/** Build the lit runs for one (category, side, heading). Same sampling as the old
 *  per-draw loop, so the pixels are identical for the same heading. */
void buildSpans(const uint8_t* alpha, int side, float heading_deg, std::vector<Span>* out) {
  out->clear();
  const float src_side = static_cast<float>(data::aircraft_icons::kIconSide);
  const float src_half = (src_side - 1.0f) * 0.5f;
  const float scale = static_cast<float>(side) / src_side;
  const float inv_scale = 1.0f / scale;

  const float rad = heading_deg * kDegToRad;
  const float sin_h = sinf(rad);
  const float cos_h = cosf(rad);
  const int radius = (side + 1) / 2 + 1;

  // Destination-space sample: every screen pixel is filled (no sparse upscale holes).
  // Max of nearest 4 source cells keeps thin rotor strokes from sparkling under rotate.
  for (int dy = -radius; dy <= radius; ++dy) {
    int run_start = 0;
    bool in_run = false;
    for (int dx = -radius; dx <= radius + 1; ++dx) {
      bool lit = false;
      if (dx <= radius) {
        // Inverse of mapLocal (screen offset → icon-local).
        const float lx = static_cast<float>(dx) * cos_h + static_cast<float>(dy) * sin_h;
        const float ly = -static_cast<float>(dx) * sin_h + static_cast<float>(dy) * cos_h;
        const float ix_f = lx * inv_scale + src_half;
        const float iy_f = ly * inv_scale + src_half;
        const int ix0 = static_cast<int>(floorf(ix_f));
        const int iy0 = static_cast<int>(floorf(iy_f));
        uint8_t a = 0;
        for (int oy = 0; oy <= 1; ++oy) {
          for (int ox = 0; ox <= 1; ++ox) {
            const int ix = ix0 + ox;
            const int iy = iy0 + oy;
            if (ix < 0 || iy < 0 || ix >= data::aircraft_icons::kIconSide ||
                iy >= data::aircraft_icons::kIconSide) {
              continue;
            }
            const uint8_t s = pgm_read_byte(
                &alpha[static_cast<size_t>(iy) * data::aircraft_icons::kIconSide +
                       static_cast<size_t>(ix)]);
            if (s > a) {
              a = s;
            }
          }
        }
        lit = a >= data::aircraft_icons::kAlphaFloor;
      }
      if (lit && !in_run) {
        run_start = dx;
        in_run = true;
      } else if (!lit && in_run) {
        out->push_back(Span{static_cast<int8_t>(dy), static_cast<int8_t>(run_start),
                            static_cast<uint8_t>(dx - run_start)});
        in_run = false;
      }
    }
  }
}

/** Cached runs for this icon, building (and evicting the least recently used) on a
 *  miss. False when the size is outside what a Span can hold or memory ran out. */
bool masksFor(uint8_t category, const uint8_t* alpha, int side, int heading_step,
              SpanView* view, bool* built) {
  *built = false;
  // Span offsets are int8: radius = (side + 1) / 2 + 1 must stay below 127.
  if (side > 240) {
    return false;
  }
  ++s_mask_clock;
  MaskEntry* victim = &s_masks[0];
  for (MaskEntry& e : s_masks) {
    if (e.used && e.category == category && e.side == side &&
        e.heading_step == heading_step) {
      e.stamp = s_mask_clock;
      *view = SpanView{e.spans, e.count};
      return true;
    }
    if (!e.used) {
      if (victim->used) {
        victim = &e;
      }
    } else if (victim->used && e.stamp < victim->stamp) {
      victim = &e;
    }
  }
  if (s_build.capacity() == 0) {
    s_build.reserve(1024);
  }
  buildSpans(alpha, side, static_cast<float>(heading_step * kHeadingStepDeg), &s_build);
  const size_t n = s_build.size();
  if (n > 0xFFFF) {
    return false;
  }
  if (n > victim->capacity) {
    heap_caps_free(victim->spans);
    victim->spans = nullptr;
    victim->capacity = 0;
    victim->used = false;
    const size_t bytes = n * sizeof(Span);
    void* mem = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem == nullptr) {
      mem = heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
    }
    if (mem == nullptr) {
      return false;
    }
    victim->spans = static_cast<Span*>(mem);
    victim->capacity = static_cast<uint16_t>(n);
  }
  if (n > 0) {
    memcpy(victim->spans, s_build.data(), n * sizeof(Span));
  }
  victim->count = static_cast<uint16_t>(n);
  victim->used = true;
  victim->category = category;
  victim->side = static_cast<uint8_t>(side);
  victim->heading_step = static_cast<uint8_t>(heading_step);
  victim->stamp = s_mask_clock;
  *built = true;
  *view = SpanView{victim->spans, victim->count};
  return true;
}

}  // namespace

bool draw(PlaneGfx& gfx, int cx, int cy, float heading_deg, uint16_t color,
          uint8_t category, int base_side_px) {
  if (!hasIcon(category) || base_side_px < 8) {
    return false;
  }
  const uint32_t t0 = micros();

  const data::aircraft_icons::IconBlob* blob = &data::aircraft_icons::kIcons[category];
  const uint8_t* alpha =
      reinterpret_cast<const uint8_t*>(pgm_read_ptr(&blob->alpha));
  const uint16_t scale_x100 = pgm_read_word(&blob->scale_x100);
  const int side =
      std::max(8, static_cast<int>((base_side_px * static_cast<int>(scale_x100) + 50) / 100));

  float heading = std::isfinite(heading_deg) ? heading_deg : 0.0f;
  int heading_step =
      static_cast<int>(lroundf(heading / static_cast<float>(kHeadingStepDeg))) % kHeadingSteps;
  if (heading_step < 0) {
    heading_step += kHeadingSteps;
  }

  bool built = false;
  SpanView view{nullptr, 0};
  if (!masksFor(category, alpha, side, heading_step, &view, &built)) {
    return false;  // absurd size or out of memory: caller draws the vector fallback
  }
  const uint32_t t_built = micros();

  uint16_t* fb = gfx.framebuffer();
  if (fb != nullptr) {
    // Sprite / offscreen canvas: write the runs straight into its pixels.
    const int fb_w = gfx.framebufferWidth();
    const int fb_h = gfx.framebufferHeight();
    for (const Span& s : view) {
      const int y = cy + s.dy;
      if (y < 0 || y >= fb_h) {
        continue;
      }
      int x0 = cx + s.x0;
      int x1 = x0 + s.len;
      if (x0 < 0) {
        x0 = 0;
      }
      if (x1 > fb_w) {
        x1 = fb_w;
      }
      if (x0 < x1) {
        std::fill_n(fb + static_cast<size_t>(y) * static_cast<size_t>(fb_w) +
                        static_cast<size_t>(x0),
                    static_cast<size_t>(x1 - x0), color);
      }
    }
  } else if (Arduino_TFT::pixelAlign2()) {
    // Panels that snap writes to a 2x2 grid (CO5300): keep the old pixel-by-pixel
    // writes so the snapping comes out exactly as before.
    for (const Span& s : view) {
      for (int i = 0; i < s.len; ++i) {
        gfx.fillRect(static_cast<int16_t>(cx + s.x0 + i), static_cast<int16_t>(cy + s.dy), 1,
                     1, color);
      }
    }
  } else {
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    gfx.startWrite();  // P4 framebuffer bus: one cache write-back for the whole icon
#endif
    for (const Span& s : view) {
      gfx.fillRect(static_cast<int16_t>(cx + s.x0), static_cast<int16_t>(cy + s.dy),
                   static_cast<int16_t>(s.len), 1, color);
    }
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    gfx.endWrite();
#endif
  }

  const uint32_t t1 = micros();
  gfx_perf::add(gfx_perf::Kind::Icon, t1 - t0);
  if (built) {
    gfx_perf::add(gfx_perf::Kind::IconMiss, t_built - t0);
  }
  return true;
}

bool isHelicopterIcon(uint8_t category) {
  using data::aircraft_icons::Category;
  return category == static_cast<uint8_t>(Category::helicopter) ||
         category == static_cast<uint8_t>(Category::military_helicopter);
}

bool isGeneralAviationIcon(uint8_t category) {
  using data::aircraft_icons::Category;
  return category == static_cast<uint8_t>(Category::small_prop_single) ||
         category == static_cast<uint8_t>(Category::small_prop_twin) ||
         category == static_cast<uint8_t>(Category::business_jet) ||
         category == static_cast<uint8_t>(Category::glider) ||
         category == static_cast<uint8_t>(Category::balloon);
}

}  // namespace ui::aircraft_icon
