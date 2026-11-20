#include "ui/radar_display.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
#include "hardware/buzzer.h"
#endif
#include "hardware/panel.h"
#include "ui/display_prefs.h"
#include "services/adsb_client.h"
#include "services/aircraft_alert.h"
#include "services/aircraft_type_lookup.h"
#include "services/https_lock.h"
#include "services/radar_basemap.h"
#include "services/rain_radar.h"
#include "geo/flat_earth.h"
#include "services/map_center.h"
#include "ui/aircraft_symbol.h"
#include "ui/alert_flash.h"
#include "ui/label_layout.h"
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
#include "ui/overhead_sign.h"
#endif
#include "ui/radar_accent.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
#include "ui/sky_banner.h"
#endif

#include "hardware/gfx_log.h"
#include "ui/ui_scale.h"
#include "util/big_static.h"

#include <esp_heap_caps.h>

namespace ui {
namespace radar {

uint16_t kColorBackground = 0x0000;
uint16_t kColorGrid = 0x0320;
uint16_t kColorSweep = 0x0320;
uint16_t kColorSweepTrail = 0x0320;
uint16_t kColorLabel = 0xFFFF;
uint16_t kColorAircraft = 0x001F;
uint16_t kColorTagType = 0x5DFF;
uint16_t kColorTagAltitudeAscend = 0x07FF;
uint16_t kColorTagAltitudeDescend = 0xF81F;
uint16_t kColorAlertMilitary = 0xFBE0;    // orange
uint16_t kColorAlertEmergency = 0xF800;   // red
uint16_t kColorTypeMilitary = 0xF800;
uint16_t kColorTypeHelicopter = 0x055F;
uint16_t kColorTypePrivate = 0xFFFF;

}  // namespace radar

namespace {

bool s_label_metrics_ready = false;
UiTextStyle s_cardinal_style = displayFontCardinal();
UiTextStyle s_scale_style = displayFontScale();
UiTextStyle s_tag_style = displayFontTag();

int s_scale_label_max_w = 0;
int s_scale_label_h = 0;

PlaneGfx* s_draw = &tft;
PlaneGfxSprite s_bg(&tft);
PlaneGfxSprite s_content(&tft);
bool s_bg_ready = false;
bool s_content_ready = false;
/** True after basemap upload/clear until background is rebuilt with the new map. */
bool s_basemap_bg_stale = false;

struct IntRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;

  IntRect() = default;
  IntRect(int x_in, int y_in, int w_in, int h_in) : x(x_in), y(y_in), w(w_in), h(h_in) {}
};

bool s_content_base_valid = false;
float s_display_sweep_deg = 0.0f;
unsigned long s_last_sweep_paint_ms = 0;
bool s_display_sweep_init = false;

struct CachedAircraftMarker {
  services::adsb::Aircraft plane{};
  int x = 0;
  int y = 0;
  bool beyond_dot = false;
  /** Tag box as resolved for this marker; empty when no tag is drawn. Stored
   *  rather than recomputed so a marker saved under an old layout still erases
   *  exactly where its text actually landed. */
  label_layout::Box tag_box{};
  /** How the marker looks, from its view: a plane that sits still but changes color,
   *  icon, highlight or tag text must still be pushed to the panel. */
  uint16_t color = 0;
  uint8_t look = 0;        ///< bit0 highlighted, bit1 notable, bit2 emergency
  uint32_t text_hash = 0;  ///< both tag lines
};

static_assert(services::adsb::kMaxAircraft <= label_layout::kMaxTargets,
              "label_layout would silently drop tags for the extra aircraft");

using MarkerArray = CachedAircraftMarker[services::adsb::kMaxAircraft];
FS_BIG_STATIC(MarkerArray, s_prev_aircraft_markers)
#define s_prev_aircraft_markers FS_BIG_STATIC_REF(s_prev_aircraft_markers)
size_t s_prev_aircraft_marker_count = 0;
/** Scratch for ADS-B refresh — must not live on loopTask stack (~8 KB). */
FS_BIG_STATIC(MarkerArray, s_current_aircraft_markers)
#define s_current_aircraft_markers FS_BIG_STATIC_REF(s_current_aircraft_markers)
bool s_marker_prev_used[services::adsb::kMaxAircraft] = {};

class DrawScope {
 public:
  explicit DrawScope(PlaneGfx& gfx) : prev_(s_draw) { s_draw = &gfx; }
  ~DrawScope() { s_draw = prev_; }

 private:
  PlaneGfx* prev_;
};

UiTextStyle pickTextSizeForHeight(int target_px, size_t lo, size_t hi) {
  return displayFontPickForHeight(tft, target_px, lo, hi);
}

void initLabelMetrics() {
  if (s_label_metrics_ready) {
    return;
  }

  s_cardinal_style = pickTextSizeForHeight(radar::kCardinalLabelHeightPx, 2, 5);
  const int cardinal_h = displayFontHeight(tft, s_cardinal_style);
  s_scale_style =
      pickTextSizeForHeight(cardinal_h - radar::kScaleBelowCardinalPx, 0, 3);
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  s_tag_style = displayFontTagLevel(0);
#else
  s_tag_style = pickTextSizeForHeight(radar::kAircraftTagLabelHeightPx, 2, 4);
#endif

  displayFontApply(tft, s_scale_style);
  s_scale_label_h = tft.fontHeight();
  s_scale_label_max_w = 0;
  char label[12];
  for (size_t i = 0; i < radar::kRangeMileOptionCount; ++i) {
    const float label_km =
        static_cast<float>(radar::kRangeMileOptions[i]) * radar::kStatuteMileKm;
    for (int ring = 1; ring <= radar::kRingCount; ++ring) {
      const float ring_km = label_km * static_cast<float>(ring) /
                            static_cast<float>(radar::kRingCount);
      for (int unit = 0; unit <= static_cast<int>(radar::DistanceUnit::NauticalMile);
           ++unit) {
        radar::formatScaleTag(label, sizeof(label), ring_km,
                              static_cast<radar::DistanceUnit>(unit));
        const int w = tft.textWidth(label);
        if (w > s_scale_label_max_w) {
          s_scale_label_max_w = w;
        }
      }
    }
  }

  s_label_metrics_ready = true;
}

void initPalette() {
  const radar::AccentRgb accent = radar::accentPalette();
  const auto bm_style = services::basemap::storedStyle();
  const bool light_map =
      services::basemap::wantsDisplay() &&
      (bm_style == services::basemap::Style::Light ||
       bm_style == services::basemap::Style::Vfr ||
       bm_style == services::basemap::Style::Voyager);

  if (light_map) {
    // Cream chips for compass/scale text (avoid CRT-green “black” boxes on Positron).
    radar::kColorBackground = tft.color565(245, 245, 238);
    // Darken accent grid/labels for contrast on pale land/water tiles.
    auto darken = [](uint8_t c) -> uint8_t {
      return static_cast<uint8_t>((static_cast<unsigned>(c) * 20u) / 100u);
    };
    radar::kColorGrid =
        tft.color565(darken(accent.grid_r), darken(accent.grid_g), darken(accent.grid_b));
    radar::kColorLabel =
        tft.color565(darken(accent.label_r), darken(accent.label_g), darken(accent.label_b));
    // Warm amber washes out on Positron — use deep crimson for blips.
    radar::kColorAircraft = tft.color565(170, 28, 36);
    radar::kColorTagType = tft.color565(120, 70, 0);
    radar::kColorTagAltitudeAscend = tft.color565(0, 110, 140);
    radar::kColorTagAltitudeDescend = tft.color565(150, 0, 120);
    // Type colors on pale maps: darker versions of the same hues.
    radar::kColorTypeMilitary = tft.color565(205, 0, 0);
    radar::kColorTypeHelicopter = tft.color565(0, 80, 210);
    radar::kColorTypePrivate = tft.color565(25, 25, 25);
    if (services::alert::typeColorsEnabled()) {
      // Crimson is too close to the military red; airliners go deep amber instead.
      radar::kColorAircraft = tft.color565(165, 80, 0);
    }
  } else {
    radar::kColorBackground = tft.color565(radar::kBgR, radar::kBgG, radar::kBgB);
    radar::kColorGrid = tft.color565(accent.grid_r, accent.grid_g, accent.grid_b);
    radar::kColorLabel = tft.color565(accent.label_r, accent.label_g, accent.label_b);
    radar::kColorAircraft =
        tft.color565(radar::kAircraftR, radar::kAircraftG, radar::kAircraftB);
    radar::kColorTagType =
        tft.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
    radar::kColorTagAltitudeAscend =
        tft.color565(radar::kTagAltAscendR, radar::kTagAltAscendG, radar::kTagAltAscendB);
    radar::kColorTagAltitudeDescend = tft.color565(radar::kTagAltDescendR,
                                                   radar::kTagAltDescendG,
                                                   radar::kTagAltDescendB);
    radar::kColorTypeMilitary = tft.color565(255, 50, 40);
    radar::kColorTypeHelicopter = tft.color565(64, 170, 255);
    radar::kColorTypePrivate = tft.color565(235, 235, 235);
  }
  // Sweep matches rings / crosshairs (same accent as the static grid).
  radar::kColorSweep = radar::kColorGrid;
  radar::kColorSweepTrail = radar::kColorGrid;
}

/** Treat small negative rates as level (cyan). */
constexpr int16_t kDescendRateThresholdFpm = -64;

void localOffsetFromCenter(float lat, float lon, float* dx_km, float* dy_km,
                           float* dist_km) {
  geo::localOffsetKm(services::map_center::latitude(), services::map_center::longitude(),
                     lat, lon, dx_km, dy_km, dist_km);
}

/** Rotate east/north so geographic facingDeg() maps to screen-up. */
void rotateEastNorthForFacing(float east_km, float north_km, float* out_east,
                              float* out_north) {
  // Facing rarely changes: keep its sin/cos instead of recomputing them for every
  // plane on every rebuild.
  static int s_cached_facing = -1;
  static float s_cos = 1.0f;
  static float s_sin = 0.0f;
  const int facing = static_cast<int>(radar::facingDeg());
  if (facing != s_cached_facing) {
    const float f = static_cast<float>(facing) * 3.14159265f / 180.0f;
    s_cos = cosf(f);
    s_sin = sinf(f);
    s_cached_facing = facing;
  }
  const float c = s_cos;
  const float s = s_sin;
  *out_east = east_km * c - north_km * s;
  *out_north = east_km * s + north_km * c;
}

/** Track/nose angle for drawing after facing rotation. */
float displayTrackDeg(float track_deg) {
  float t = track_deg - static_cast<float>(radar::facingDeg());
  while (t < 0.0f) {
    t += 360.0f;
  }
  while (t >= 360.0f) {
    t -= 360.0f;
  }
  return t;
}

float innerRingMaxKm() {
  const float outer_km = radar::scaleActive().label_km;
  return outer_km * (static_cast<float>(radar::kGridOuterRadius -
                                       radar::kAircraftInsideRingInsetPx) /
                     static_cast<float>(radar::kGridOuterRadius));
}

bool isInsideOuterRingKm(float dist_km) { return dist_km <= innerRingMaxKm(); }

int distSqFromCenter(int x, int y) {
  const int dx = x - radar::kCenterX;
  const int dy = y - radar::kCenterY;
  return dx * dx + dy * dy;
}

/** Icon color: amber airliners by default; with type colors on, military red,
 *  helicopters blue and private/GA white (darker versions on light maps). */
uint16_t aircraftBaseColor(const services::adsb::Aircraft& ac) {
  if (!services::alert::typeColorsEnabled()) {
    return radar::kColorAircraft;
  }
  switch (services::alert::trafficClass(ac)) {
    case services::alert::TrafficClass::Military:
      return radar::kColorTypeMilitary;
    case services::alert::TrafficClass::Helicopter:
      return radar::kColorTypeHelicopter;
    case services::alert::TrafficClass::Private:
      return radar::kColorTypePrivate;
    default:
      return radar::kColorAircraft;
  }
}

void applyTagStyle() { displayFontApply(*s_draw, s_tag_style); }

int aircraftSymbolHalfPx() {
  return std::max(aircraft_symbol::radiusPx(), radar::kAircraftIconRadiusPx);
}

/**
 * Tag text: maker on top, short model below ("Cessna" / "172 Skyhawk"). No tail
 * number or altitude: those are on the flight detail and the selected-plane banner.
 * Unknown type: the ICAO code, else the callsign, so a plane is never anonymous.
 * Notable aircraft show their friendly name ("Hurricane Hunter") as the second line.
 */
struct TagText {
  char line1[24] = "";
  char line2[32] = "";
};

TagText tagText(const services::adsb::Aircraft& plane, const char* notable_tag) {
  TagText t;
  if (!services::aircraft_type::formatMakeModel(plane.type, t.line1, sizeof(t.line1), t.line2,
                                                sizeof(t.line2))) {
    t.line1[0] = '\0';
    t.line2[0] = '\0';
    if (plane.type[0] != '\0') {
      services::aircraft_type::formatRadarTagLabel(plane.type, t.line1, sizeof(t.line1));
    } else if (plane.callsign[0] != '\0') {
      snprintf(t.line1, sizeof(t.line1), "%s", plane.callsign);
    }
    if (strcmp(plane.alt, "GND") == 0) {
      snprintf(t.line2, sizeof(t.line2), "ground");
    }
  }
  if (notable_tag != nullptr) {
    snprintf(t.line2, sizeof(t.line2), "%s", notable_tag);
  }
  if (t.line1[0] == '\0' && t.line2[0] != '\0') {
    snprintf(t.line1, sizeof(t.line1), "%s", t.line2);
    t.line2[0] = '\0';
  }
  return t;
}

/** FNV-1a of both lines, so a marker can tell its tag text changed. */
uint32_t tagTextHash(const TagText& t) {
  uint32_t h = 2166136261u;
  const char* const lines[] = {t.line1, t.line2};
  for (const char* s : lines) {
    for (; *s != '\0'; ++s) {
      h = (h ^ static_cast<uint8_t>(*s)) * 16777619u;
    }
    h = (h ^ 0xFFu) * 16777619u;  // line break: "AB"+"C" differs from "A"+"BC"
  }
  return h;
}

// Layout is measured against the panel, never the sprite currently being drawn
// into: the resolver caches on a signature built from these widths, so metrics
// that differed by target would re-solve (and jitter) on every rebuild.
int tagLineHeight() {
  displayFontApply(tft, s_tag_style);
  return tft.fontHeight();
}

// --- Per-plane view cache --------------------------------------------------------
// Everything a rebuild needs per plane, worked out once per ADS-B poll (and after a
// settings or alert change) instead of on every rebuild: screen position, ring test,
// highlight, color, tag text, its widths and its resolved box. Before this, each
// rebuild parsed the type name 4x per plane, ran the alert match 3-4x and the
// double-precision flat-earth offset 8-10x.

struct PlaneView {
  uint16_t index = 0;        ///< into services::adsb::aircraftList()
  bool in_range = false;     ///< full symbol inside the outer ring
  bool beyond = false;       ///< small icon on the rim
  bool highlighted = false;  ///< blinks (alert classes, watch list, notable...)
  bool notable = false;
  bool emergency = false;
  bool tag_on_right = false;
  uint8_t tag_tier = label_layout::kTierHidden;
  uint8_t lines = 0;
  int16_t x = 0;
  int16_t y = 0;
  int16_t full_w = 0;
  int16_t short_w = 0;
  int32_t dist_sq = 0;
  float disp_track = 0.0f;
  uint16_t base_color = 0;
  label_layout::Box tag_box{};  ///< empty when no tag is drawn
  TagText text;
};

using ViewArray = PlaneView[services::adsb::kMaxAircraft];
FS_BIG_STATIC(ViewArray, s_views)
#define s_views FS_BIG_STATIC_REF(s_views)
size_t s_view_count = 0;
/** Draw order, farthest from center first (closest lands on top). */
uint8_t s_dot_order[services::adsb::kMaxAircraft];
size_t s_dot_order_count = 0;
uint8_t s_sym_order[services::adsb::kMaxAircraft];
size_t s_sym_order_count = 0;
bool s_views_valid = false;
uint32_t s_views_sig = 0;

/** Luma at or above this is a light map; tag ink switches to dark. */
constexpr uint8_t kTagLightBackgroundLuma = 140;

struct TagInk {
  uint16_t label = 0;
  uint16_t type = 0;
  uint16_t ascend = 0;
  uint16_t descend = 0;
};

TagInk tagInkForBackground(bool light_background) {
  TagInk ink;
  if (light_background) {
    ink.label = tft.color565(radar::kTagTextLightR, radar::kTagTextLightG, radar::kTagTextLightB);
    ink.type = tft.color565(radar::kTagTypeLightR, radar::kTagTypeLightG, radar::kTagTypeLightB);
    ink.ascend = tft.color565(radar::kTagAltAscendLightR, radar::kTagAltAscendLightG,
                              radar::kTagAltAscendLightB);
    ink.descend = tft.color565(radar::kTagAltDescendLightR, radar::kTagAltDescendLightG,
                               radar::kTagAltDescendLightB);
  } else {
    ink.label = tft.color565(radar::kTagTextDarkR, radar::kTagTextDarkG, radar::kTagTextDarkB);
    ink.type = tft.color565(radar::kTagTypeR, radar::kTagTypeG, radar::kTagTypeB);
    ink.ascend =
        tft.color565(radar::kTagAltAscendR, radar::kTagAltAscendG, radar::kTagAltAscendB);
    ink.descend = tft.color565(radar::kTagAltDescendR, radar::kTagAltDescendG,
                               radar::kTagAltDescendB);
  }
  return ink;
}

bool tagSitsOnLightBackground(const label_layout::Box& box) {
  uint8_t luma = 0;
  // Judge the map under the tag (background sprite), not what is already drawn
  // there: a region rebuild still has the old frame around it, and both kinds of
  // rebuild must pick the same ink.
  const PlaneGfx& src = s_bg_ready ? s_bg.gfx() : *s_draw;
  if (src.averageLuma(box.x, box.y, box.w, box.h, &luma)) {
    return luma >= kTagLightBackgroundLuma;
  }
  const auto bm_style = services::basemap::storedStyle();
  return services::basemap::wantsDisplay() &&
         (bm_style == services::basemap::Style::Light ||
          bm_style == services::basemap::Style::Vfr ||
          bm_style == services::basemap::Style::Voyager);
}

[[maybe_unused]] uint16_t altitudeInk(const TagInk& ink, const services::adsb::Aircraft& plane) {
  if (strcmp(plane.alt, "GND") == 0) {
    return ink.type;
  }
  if (plane.vert_rate_fpm != services::adsb::kVertRateUnknown &&
      plane.vert_rate_fpm < kDescendRateThresholdFpm) {
    return ink.descend;
  }
  return ink.ascend;
}

void drawTagForView(const PlaneView& v) {
  applyTagStyle();

  const int line_h = s_draw->fontHeight();
  // Text stays flush against the edge facing the symbol, so the ragged edge is
  // the one pointing away from the traffic it belongs to.
  const int anchor_x = v.tag_on_right ? v.tag_box.x : v.tag_box.x + v.tag_box.w;
  s_draw->setTextDatum(v.tag_on_right ? TextDatum::TopLeft : TextDatum::TopRight);
  int ly = v.tag_box.y;
  // Ink follows the map under the tag. A single color (no background) keeps
  // the glyph transparent so the basemap shows through instead of a solid chip.
  const bool light_bg = tagSitsOnLightBackground(v.tag_box);
  const TagInk ink = tagInkForBackground(light_bg);

  const TagText& text = v.text;
  if (text.line1[0] != '\0') {
    s_draw->setTextColor(ink.label);
    s_draw->drawString(text.line1, anchor_x, ly);
  }
  if (v.tag_tier != label_layout::kTierFull || text.line2[0] == '\0') {
    return;
  }
  ly += line_h;

  uint16_t type_ink = ink.type;
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  if (v.notable) {
    // Notable name in an ink no other tag line uses: white on dark maps, crimson on
    // light maps.
    type_ink = light_bg ? tft.color565(190, 0, 60) : tft.color565(255, 255, 255);
  }
#endif
  s_draw->setTextColor(type_ink);
  s_draw->drawString(text.line2, anchor_x, ly);
}

/** Per aircraft-list index, filled by fillAircraftLabelKeys(). */
uint32_t s_aircraft_label_keys[services::adsb::kMaxAircraft];

/** Identity that survives the aircraft array being rebuilt on every poll. */
uint32_t aircraftLabelBaseKey(const services::adsb::Aircraft& plane) {
  char id[64];
  if (plane.hex[0] != '\0') {
    snprintf(id, sizeof(id), "h:%s", plane.hex);
  } else if (plane.registration[0] != '\0') {
    snprintf(id, sizeof(id), "r:%s", plane.registration);
  } else if (plane.callsign[0] != '\0') {
    snprintf(id, sizeof(id), "c:%s", plane.callsign);
  } else {
    // Coarse bucket so a tag stays put while an unidentified aircraft creeps.
    // Shared buckets are split in fillAircraftLabelKeys().
    snprintf(id, sizeof(id), "p:%.3f,%.3f", static_cast<double>(plane.lat),
             static_cast<double>(plane.lon));
  }
  return label_layout::hashKey(id);
}

void fillAircraftLabelKeys(const services::adsb::Aircraft* planes, size_t n) {
  if (planes == nullptr || n == 0) {
    return;
  }
  if (n > services::adsb::kMaxAircraft) {
    n = services::adsb::kMaxAircraft;
  }
  static int32_t sort_lat[services::adsb::kMaxAircraft];
  static int32_t sort_lon[services::adsb::kMaxAircraft];
  for (size_t i = 0; i < n; ++i) {
    s_aircraft_label_keys[i] = aircraftLabelBaseKey(planes[i]);
    sort_lat[i] = static_cast<int32_t>(lroundf(planes[i].lat * 100000.0f));
    sort_lon[i] = static_cast<int32_t>(lroundf(planes[i].lon * 100000.0f));
  }
  label_layout::uniquifyKeys(s_aircraft_label_keys, sort_lat, sort_lon, n);
}

bool linkBadgeBox(IntRect* out);
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
bool rainLabelRect(IntRect* out);
bool muteIconShown();
IntRect muteIconRect();
#endif

void addBlocked(label_layout::Geometry* geom, int x, int y, int w, int h) {
  if (geom->blocked_count >= label_layout::kMaxBlocked || w <= 0 || h <= 0) {
    return;
  }
  geom->blocked[geom->blocked_count++] =
      label_layout::Box{static_cast<int16_t>(x), static_cast<int16_t>(y),
                        static_cast<int16_t>(w), static_cast<int16_t>(h)};
}

label_layout::Geometry labelGeometry() {
  label_layout::Geometry geom;
  geom.center_x = radar::kCenterX;
  geom.center_y = radar::kCenterY;
  geom.visible_radius = radar::kAircraftLabelVisibleRadiusPx;
  geom.symbol_half = static_cast<int16_t>(aircraftSymbolHalfPx());
  geom.gap = radar::kAircraftLabelGapPx;
  // Fixed overlays and labels no tag may cover: tags move off them, shrink or hide
  // rather than print over them or show through them.
  IntRect r;
  if (linkBadgeBox(&r)) {
    addBlocked(&geom, r.x, r.y, r.w, r.h);
  }
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  int bx = 0, by = 0, bw = 0, bh = 0;
  if (ui::sky::bannerBounds(&bx, &by, &bw, &bh)) {
    addBlocked(&geom, bx, by, bw, bh);
  }
  if (overheadSignBounds(&bx, &by, &bw, &bh)) {
    addBlocked(&geom, bx, by, bw, bh);
  }
  if (rainLabelRect(&r)) {
    addBlocked(&geom, r.x, r.y, r.w, r.h);
  }
  if (muteIconShown()) {
    r = muteIconRect();
    addBlocked(&geom, r.x, r.y, r.w, r.h);
  }
#endif
  return geom;
}

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
// Busy sky: smaller tag text so more tags fit before the layout starts hiding them.
// Level 0 = normal, 1 = smaller, 2 = smallest. Hysteresis keeps a count hovering
// at a threshold from flipping the size every poll.
constexpr size_t kTagDenserAbove[] = {8, 14};  // go to level 1 / 2 above these
constexpr size_t kTagRoomierAt[] = {6, 11};    // back to level 0 / 1 at or below these
UiTextStyle s_tag_styles[3];
bool s_tag_styles_ready = false;
uint8_t s_tag_level = 0;

void updateTagDensity(size_t n) {
  if (!s_tag_styles_ready) {
    s_tag_styles[0] = displayFontTagLevel(0);  // 14 pt
    s_tag_styles[1] = displayFontTagLevel(1);  // 12 pt
    s_tag_styles[2] = displayFontTagLevel(2);  // 10 pt
    s_tag_styles_ready = true;
    Serial.printf("[radar] tag text heights: %d / %d / %d px\n",
                  displayFontHeight(tft, s_tag_styles[0]), displayFontHeight(tft, s_tag_styles[1]),
                  displayFontHeight(tft, s_tag_styles[2]));
  }
  uint8_t level = s_tag_level;
  while (level < 2 && n > kTagDenserAbove[level]) {
    ++level;
  }
  while (level > 0 && n <= kTagRoomierAt[level - 1]) {
    --level;
  }
  if (level == s_tag_level) {
    return;
  }
  Serial.printf("[radar] %u planes: tag text level %u -> %u\n", static_cast<unsigned>(n),
                static_cast<unsigned>(s_tag_level), static_cast<unsigned>(level));
  s_tag_level = level;
  s_tag_style = s_tag_styles[level];
  // Every tag changes size: repaint the whole aircraft layer, not just moved markers.
  radarDisplayInvalidateAircraft();
}
#endif

/** Cheap fingerprint of everything the views are built from. A mismatch means the
 *  aircraft list or a setting changed without a full rebuild noticing. */
uint32_t viewsSignature() {
  uint32_t h = 2166136261u;
  auto mix = [&h](const void* p, size_t len) {
    const uint8_t* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < len; ++i) {
      h ^= b[i];
      h *= 16777619u;
    }
  };
  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  mix(&n, sizeof(n));
  for (size_t i = 0; i < n && planes != nullptr; ++i) {
    const services::adsb::Aircraft& a = planes[i];
    mix(&a.lat, sizeof(a.lat));
    mix(&a.lon, sizeof(a.lon));
    mix(&a.track_deg, sizeof(a.track_deg));
    mix(a.hex, sizeof(a.hex));
    mix(a.type, sizeof(a.type));
    mix(a.alt, sizeof(a.alt));
    mix(a.callsign, sizeof(a.callsign));
    mix(a.squawk, sizeof(a.squawk));
    mix(a.category, sizeof(a.category));
    mix(&a.db_flags, sizeof(a.db_flags));
  }
  const uint16_t facing = radar::facingDeg();
  mix(&facing, sizeof(facing));
  const float label_km = radar::scaleActive().label_km;
  mix(&label_km, sizeof(label_km));
  const double clat = services::map_center::latitude();
  const double clon = services::map_center::longitude();
  mix(&clat, sizeof(clat));
  mix(&clon, sizeof(clon));
  const uint8_t flags = (ui::displayPrefsHideBlipDetails() ? 1 : 0) |
                        (services::alert::typeColorsEnabled() ? 2 : 0);
  mix(&flags, sizeof(flags));
  return h;
}

void sortOrderFarFirst(uint8_t* order, size_t count) {
  for (size_t i = 1; i < count; ++i) {
    const uint8_t key = order[i];
    const int32_t key_d = s_views[key].dist_sq;
    size_t j = i;
    while (j > 0 && s_views[order[j - 1]].dist_sq < key_d) {
      order[j] = order[j - 1];
      --j;
    }
    order[j] = key;
  }
}

/**
 * Rebuild the per-plane views and place every visible tag as a set. Runs once per
 * ADS-B poll and on full rebuilds; region rebuilds (alert pulse, banner, toast)
 * reuse the result so their pixels match the rest of the frame exactly.
 */
void refreshPlaneViews() {
  initLabelMetrics();
  const size_t n = std::min(services::adsb::aircraftCount(), services::adsb::kMaxAircraft);
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();

  const float outer_km = radar::scaleActive().label_km;
  const float px_per_km = static_cast<float>(radar::kGridOuterRadius) / outer_km;
  const float inner_max_km = innerRingMaxKm();
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  const int rim_r = radar::kCenterX - aircraft_symbol::compactHalfPx() - ui::sc(3);
#else
  const int rim_r = radar::kCenterX - radar::kBeyondRingScreenMarginPx;
#endif

  s_view_count = 0;
  s_dot_order_count = 0;
  s_sym_order_count = 0;
  size_t visible = 0;
  for (size_t i = 0; i < n && planes != nullptr; ++i) {
    const services::adsb::Aircraft& p = planes[i];
    if (services::alert::hiddenOnRadar(p)) {
      continue;
    }
    float dx_km = 0.0f;
    float dy_km = 0.0f;
    float dist_km = 0.0f;
    localOffsetFromCenter(p.lat, p.lon, &dx_km, &dy_km, &dist_km);
    const bool inside = dist_km <= inner_max_km;
    if (!inside && dist_km < 0.01f) {
      continue;
    }
    float ex = 0.0f;
    float ny = 0.0f;
    rotateEastNorthForFacing(dx_km, dy_km, &ex, &ny);

    PlaneView& v = s_views[s_view_count];
    v = PlaneView{};
    v.index = static_cast<uint16_t>(i);
    v.in_range = inside;
    v.beyond = !inside;
    if (inside) {
      v.x = static_cast<int16_t>(radar::kCenterX + static_cast<int>(lroundf(ex * px_per_km)));
      v.y = static_cast<int16_t>(radar::kCenterY - static_cast<int>(lroundf(ny * px_per_km)));
      ++visible;
    } else {
      // The whole small icon stays on the glass, on the rim along its bearing.
      const float angle_rad = atan2f(ex, ny);
      v.x = static_cast<int16_t>(radar::kCenterX +
                                 static_cast<int>(lroundf(sinf(angle_rad) * rim_r)));
      v.y = static_cast<int16_t>(radar::kCenterY -
                                 static_cast<int>(lroundf(cosf(angle_rad) * rim_r)));
    }
    v.dist_sq = distSqFromCenter(v.x, v.y);
    v.disp_track = displayTrackDeg(p.track_deg);
    v.base_color = aircraftBaseColor(p);
    v.emergency = p.isEmergencySquawk();
    v.highlighted = inside && services::alert::isHighlighted(p);
    const char* notable_tag = nullptr;
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    if (inside) {
      notable_tag = services::alert::notableTag(p);
    }
#endif
    v.notable = notable_tag != nullptr;
    if (inside) {
      v.text = tagText(p, notable_tag);
      s_sym_order[s_sym_order_count++] = static_cast<uint8_t>(s_view_count);
    } else {
      s_dot_order[s_dot_order_count++] = static_cast<uint8_t>(s_view_count);
    }
    ++s_view_count;
  }

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  // May change the tag font (and invalidate the layer) before anything is measured.
  updateTagDensity(visible);
#endif

  // Measure every tag once, against the panel (see tagLineHeight()).
  const int line_h = tagLineHeight();
  for (size_t s = 0; s < s_sym_order_count; ++s) {
    PlaneView& v = s_views[s_sym_order[s]];
    const int w1 = v.text.line1[0] != '\0' ? tft.textWidth(v.text.line1) : 0;
    const int w2 = v.text.line2[0] != '\0' ? tft.textWidth(v.text.line2) : 0;
    v.full_w = static_cast<int16_t>(std::max(w1, w2));
    v.short_w = static_cast<int16_t>(w1);
    v.lines = static_cast<uint8_t>((w1 > 0 ? 1 : 0) + (w2 > 0 ? 1 : 0));
  }

  const label_layout::Geometry geom = labelGeometry();
  if (ui::displayPrefsHideBlipDetails()) {
    label_layout::resolve(nullptr, 0, geom);
  } else {
    fillAircraftLabelKeys(planes, n);
    // Static: keeps 64 targets off the loop task's stack.
    static label_layout::Target targets[services::adsb::kMaxAircraft];
    size_t target_count = 0;
    for (size_t s = 0; s < s_sym_order_count; ++s) {
      const PlaneView& v = s_views[s_sym_order[s]];
      if (v.full_w <= 0) {
        continue;
      }
      label_layout::Target& t = targets[target_count++];
      t.key = s_aircraft_label_keys[v.index];
      t.x = v.x;
      t.y = v.y;
      t.full_w = v.full_w;
      t.full_h = static_cast<int16_t>(line_h * v.lines);
      t.short_w = v.short_w;
      t.short_h = v.short_w > 0 ? static_cast<int16_t>(line_h) : 0;
      // Closest to center places first, so nearby traffic keeps the good slots.
      t.priority = v.dist_sq;
    }
    label_layout::resolve(targets, target_count, geom);
    for (size_t s = 0; s < s_sym_order_count; ++s) {
      PlaneView& v = s_views[s_sym_order[s]];
      if (v.full_w <= 0) {
        continue;
      }
      const label_layout::Placement* placement = label_layout::find(s_aircraft_label_keys[v.index]);
      if (placement == nullptr || placement->tier == label_layout::kTierHidden) {
        continue;
      }
      v.tag_box = placement->box;
      v.tag_tier = placement->tier;
      v.tag_on_right = placement->on_right;
    }
  }

  sortOrderFarFirst(s_dot_order, s_dot_order_count);
  sortOrderFarFirst(s_sym_order, s_sym_order_count);
  s_views_sig = viewsSignature();
  s_views_valid = true;
}

/** Views for a region rebuild: false when they no longer match the aircraft list or
 *  settings (caller then does a full rebuild, which refreshes them). */
bool planeViewsCurrent() { return s_views_valid && viewsSignature() == s_views_sig; }

bool isAircraftInRange(const services::adsb::Aircraft& ac) {
  float dx_km = 0.0f;
  float dy_km = 0.0f;
  float dist_km = 0.0f;
  localOffsetFromCenter(ac.lat, ac.lon, &dx_km, &dy_km, &dist_km);
  return isInsideOuterRingKm(dist_km);
}

size_t inRangeAircraftCount() {
  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  size_t in_range = 0;
  for (size_t i = 0; i < n; ++i) {
    if (isAircraftInRange(planes[i])) {
      ++in_range;
    }
  }
  return in_range;
}

/** Aircraft drawn as full planes inside the outer ring (hide filter applied).
 *  Beyond-ring edge blips are excluded — idle clock only returns to radar for
 *  in-scope traffic, not off-screen dots. */
size_t visibleAircraftCount() {
  const size_t n = services::adsb::aircraftCount();
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  size_t visible = 0;
  for (size_t i = 0; i < n; ++i) {
    if (services::alert::hiddenOnRadar(planes[i])) {
      continue;
    }
    if (isAircraftInRange(planes[i])) {
      ++visible;
    }
  }
  return visible;
}

// --- Link badge ("No Wi-Fi" / "Flight data offline") -----------------------
// A pill in the lower part of the scope, drawn over the aircraft layer so it is
// part of every composed frame. Solid warning colors read on both the dark
// radar and the light map styles; only the outline and text ink change.

RadarLinkStatus s_link_status = RadarLinkStatus::Ok;

/** Vertical center of the badge: well inside the outer ring, above the "S". */
int linkBadgeCenterY() { return radar::kCenterY + (radar::kGridOuterRadius * 58) / 100; }

const char* linkBadgeText(RadarLinkStatus status) {
  switch (status) {
    case RadarLinkStatus::NoWifi:
      return "No Wi-Fi";
    case RadarLinkStatus::FeedDown:
      return "Flight data offline";
    default:
      return nullptr;
  }
}

/** The pill including its outline, while a badge is shown. */
bool linkBadgeBox(IntRect* out) {
  const char* text = linkBadgeText(s_link_status);
  if (text == nullptr) {
    return false;
  }
  const UiTextStyle style = displayFontDetail();
  displayFontApply(tft, style);
  const int h = displayFontHeight(tft, style) + ui::sc(8);
  const int w = tft.textWidth(text) + 2 * ui::sc(4) + ui::sc(7) + h;
  const int o = ui::sc(2) + 1;
  *out = IntRect{radar::kCenterX - w / 2 - o, linkBadgeCenterY() - h / 2 - o, w + 2 * o,
                 h + 2 * o};
  return true;
}

void drawLinkBadge() {
  const char* text = linkBadgeText(s_link_status);
  if (text == nullptr) {
    return;
  }
  const UiTextStyle style = displayFontDetail();
  displayFontApply(*s_draw, style);
  const int text_h = displayFontHeight(*s_draw, style);
  const int text_w = s_draw->textWidth(text);
  const int h = text_h + ui::sc(8);
  const int r = h / 2;
  const int dot_r = ui::sc(4);
  const int dot_gap = ui::sc(7);
  const int w = text_w + 2 * dot_r + dot_gap + 2 * r;
  const int x = radar::kCenterX - w / 2;
  const int y = linkBadgeCenterY() - h / 2;

  label_layout::Box box;
  box.x = static_cast<int16_t>(x);
  box.y = static_cast<int16_t>(y);
  box.w = static_cast<int16_t>(w);
  box.h = static_cast<int16_t>(h);
  const bool light = tagSitsOnLightBackground(box);
  const bool no_wifi = s_link_status == RadarLinkStatus::NoWifi;
  // Red for no Wi-Fi, amber for a feed outage; slightly deeper on light maps.
  const uint16_t fill = no_wifi ? (light ? s_draw->color565(190, 30, 30)
                                         : s_draw->color565(215, 45, 40))
                                : (light ? s_draw->color565(225, 140, 0)
                                         : s_draw->color565(255, 170, 20));
  const uint16_t ink = no_wifi ? s_draw->color565(255, 255, 255) : s_draw->color565(20, 20, 20);
  const uint16_t outline = light ? s_draw->color565(40, 40, 40) : s_draw->color565(0, 0, 0);

  // Outline, then the pill (rect + round caps).
  const int o = ui::sc(2);
  s_draw->fillCircle(x + r, y + r, r + o, outline);
  s_draw->fillCircle(x + w - r - 1, y + r, r + o, outline);
  s_draw->fillRect(x + r, y - o, w - 2 * r, h + 2 * o, outline);
  s_draw->fillCircle(x + r, y + r, r, fill);
  s_draw->fillCircle(x + w - r - 1, y + r, r, fill);
  s_draw->fillRect(x + r, y, w - 2 * r, h, fill);

  // Status dot (hollow ring = "not connected"), then the text.
  const int dot_cx = x + r + dot_r;
  s_draw->fillCircle(dot_cx, y + r, dot_r, ink);
  s_draw->fillCircle(dot_cx, y + r, dot_r - ui::sc(2) > 0 ? dot_r - ui::sc(2) : 1, fill);
  s_draw->setTextColor(ink);
  s_draw->setTextDatum(TextDatum::MiddleLeft);
  s_draw->drawString(text, dot_cx + dot_r + dot_gap, y + r);
}

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
// --- One-touch mute (4C long-press) ----------------------------------------
// A small speaker-with-slash at the left edge while Alert Sound is off, and a
// brief centered "Sound off" / "Sound on" toast after each long-press. Both are
// drawn over the aircraft layer, like the link badge, so every frame keeps them.

constexpr unsigned long kSoundToastMs = 1500;
unsigned long s_sound_toast_until_ms = 0;
bool s_sound_toast_on = false;
bool s_sound_toast_drawn = false;
bool s_mute_icon_drawn = false;

int muteIconCenterX() { return ui::sc(20); }
int muteIconRadius() { return ui::sc(12); }

IntRect muteIconRect() {
  const int r = muteIconRadius() + ui::sc(2);
  return IntRect{muteIconCenterX() - r, radar::kCenterY - r, 2 * r + 1, 2 * r + 1};
}

bool muteIconShown() { return !hardware::buzzerEnabled(); }

IntRect soundToastRect() {
  const int w = ui::sc(150);
  const int h = ui::sc(40);
  return IntRect{radar::kCenterX - w / 2, radar::kCenterY - h / 2, w, h};
}

/** Solid thick segment as a quad (two triangles); crisp at any angle. */
void fillThickSegment(int x0, int y0, int x1, int y1, float half_w, uint16_t color) {
  const float dx = static_cast<float>(x1 - x0);
  const float dy = static_cast<float>(y1 - y0);
  const float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f) {
    return;
  }
  const int nx = static_cast<int>(lroundf(-dy / len * half_w));
  const int ny = static_cast<int>(lroundf(dx / len * half_w));
  s_draw->fillTriangle(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, color);
  s_draw->fillTriangle(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, color);
}

/** Speaker (box + flared cone) about 2.4 * s wide, centered at (cx, cy).
 *  muted: a small x right of the cone; otherwise two sound waves. */
void drawSpeakerGlyph(int cx, int cy, int s, uint16_t ink, bool muted) {
  const int body_w = s * 5 / 8;
  const int body_h = s * 7 / 8;
  const int cone_w = s * 6 / 8;
  const int cone_h = s * 15 / 8;
  // Centered as a whole; with waves the speaker sits a bit left to make room.
  const int total_w = body_w + cone_w + s;
  const int left = cx - total_w / 2;
  const int bx1 = left + body_w;
  const int cx1 = bx1 + cone_w;
  s_draw->fillRect(left, cy - body_h / 2, body_w, body_h, ink);
  s_draw->fillTriangle(bx1 - 1, cy - body_h / 2, cx1, cy - cone_h / 2, cx1, cy + cone_h / 2, ink);
  s_draw->fillTriangle(bx1 - 1, cy - body_h / 2, cx1, cy + cone_h / 2, bx1 - 1, cy + body_h / 2,
                       ink);
  if (muted) {
    const int d = s / 2;
    const int xc = cx1 + s / 2 + ui::sc(1);
    const float hw = static_cast<float>(ui::sc(1)) + 0.3f;
    fillThickSegment(xc - d, cy - d, xc + d, cy + d, hw, ink);
    fillThickSegment(xc - d, cy + d, xc + d, cy - d, hw, ink);
  } else {
    // Two sound waves right of the cone (0 deg = 3 o'clock; split so no wrap past 360).
    const int ax = cx1 - ui::sc(1);
    const int t = ui::sc(2);
    const int radii[] = {s / 2 + t, s + t};
    for (int ro : radii) {
      s_draw->fillArc(ax, cy, ro, ro - t, 315.0f, 359.9f, ink);
      s_draw->fillArc(ax, cy, ro, ro - t, 0.0f, 45.0f, ink);
    }
  }
}

void drawMuteIcon() {
  if (hardware::buzzerEnabled()) {
    return;
  }
  const int cx = muteIconCenterX();
  const int cy = radar::kCenterY;
  const int r = muteIconRadius();
  // Dim dark disc so the glyph reads on the radar and on light map styles alike.
  const uint16_t disc = s_draw->color565(25, 25, 25);
  const uint16_t ink = s_draw->color565(200, 200, 200);
  s_draw->fillCircle(cx, cy, r + ui::sc(1), s_draw->color565(90, 90, 90));
  s_draw->fillCircle(cx, cy, r, disc);
  drawSpeakerGlyph(cx, cy, ui::sc(7), ink, true);
}

void drawSoundToast() {
  if (s_sound_toast_until_ms == 0) {
    return;
  }
  const IntRect rect = soundToastRect();
  const int r = rect.h / 2;
  const uint16_t fill = s_draw->color565(20, 20, 20);
  const uint16_t edge = s_draw->color565(120, 120, 120);
  const uint16_t ink = s_draw->color565(255, 255, 255);
  const int o = ui::sc(1);
  s_draw->fillCircle(rect.x + r, rect.y + r, r, edge);
  s_draw->fillCircle(rect.x + rect.w - r - 1, rect.y + r, r, edge);
  s_draw->fillRect(rect.x + r, rect.y, rect.w - 2 * r, rect.h, edge);
  s_draw->fillCircle(rect.x + r, rect.y + r, r - o, fill);
  s_draw->fillCircle(rect.x + rect.w - r - 1, rect.y + r, r - o, fill);
  s_draw->fillRect(rect.x + r, rect.y + o, rect.w - 2 * r, rect.h - 2 * o, fill);

  const char* text = s_sound_toast_on ? "Sound on" : "Sound off";
  const UiTextStyle style = displayFontBody();
  displayFontApply(*s_draw, style);
  const int glyph = ui::sc(9);
  const int gap = ui::sc(8);
  const int text_w = s_draw->textWidth(text);
  const int total = glyph * 2 + gap + text_w;
  const int x0 = radar::kCenterX - total / 2;
  const int cy = rect.y + r;
  drawSpeakerGlyph(x0 + glyph, cy, glyph * 2 / 3, ink, !s_sound_toast_on);
  s_draw->setTextColor(ink);
  s_draw->setTextDatum(TextDatum::MiddleLeft);
  s_draw->drawString(text, x0 + glyph * 2 + gap, cy);
}

#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C

// --- Rect helpers ------------------------------------------------------------------

bool rectEmpty(const IntRect& r) { return r.w <= 0 || r.h <= 0; }

IntRect clampRectToScreen(IntRect r) {
  if (r.x < 0) {
    r.w += r.x;
    r.x = 0;
  }
  if (r.y < 0) {
    r.h += r.y;
    r.y = 0;
  }
  if (r.x + r.w > radar::kSize) {
    r.w = radar::kSize - r.x;
  }
  if (r.y + r.h > radar::kSize) {
    r.h = radar::kSize - r.y;
  }
  return r;
}

IntRect unionRect(const IntRect& a, const IntRect& b) {
  if (rectEmpty(a)) {
    return b;
  }
  if (rectEmpty(b)) {
    return a;
  }
  const int x0 = std::min(a.x, b.x);
  const int y0 = std::min(a.y, b.y);
  const int x1 = std::max(a.x + a.w, b.x + b.w);
  const int y1 = std::max(a.y + a.h, b.y + b.h);
  return IntRect{x0, y0, x1 - x0, y1 - y0};
}

bool rectsOverlap(const IntRect& a, const IntRect& b) {
  return a.x < b.x + b.w && b.x < a.x + a.w && a.y < b.y + b.h && b.y < a.y + a.h;
}

bool rectContains(const IntRect& outer, const IntRect& inner) {
  return inner.x >= outer.x && inner.y >= outer.y && inner.x + inner.w <= outer.x + outer.w &&
         inner.y + inner.h <= outer.y + outer.h;
}

IntRect grownRect(const IntRect& r, int by) {
  return IntRect{r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by};
}

int rectArea(const IntRect& r) { return rectEmpty(r) ? 0 : r.w * r.h; }

// --- Dirty regions -------------------------------------------------------------------
// A list of screen rects to rebuild and blit, so an alert pulse on two planes at
// opposite edges costs two small patches instead of one rect spanning the screen.

constexpr int kMaxRegionRects = 64;

struct RegionList {
  IntRect rects[kMaxRegionRects];
  int count = 0;
  bool full = false;  ///< the whole screen

  bool empty() const { return !full && count == 0; }
  void clear() {
    count = 0;
    full = false;
  }
  int area() const {
    int a = 0;
    for (int i = 0; i < count; ++i) {
      a += rectArea(rects[i]);
    }
    return a;
  }
};

/** Merge `r` into the list. Overlapping rects fuse when their union wastes little. */
void regionAdd(RegionList* list, IntRect r) {
  r = clampRectToScreen(r);
  if (rectEmpty(r) || list->full) {
    return;
  }
  bool merged = true;
  while (merged) {
    merged = false;
    for (int i = 0; i < list->count; ++i) {
      const IntRect& o = list->rects[i];
      if (rectContains(o, r)) {
        return;
      }
      if (!rectsOverlap(grownRect(o, ui::sc(4)), r)) {
        continue;
      }
      const IntRect u = unionRect(o, r);
      // Pixels the union would add beyond the two pieces. Judged against the smaller
      // piece so a growing rect cannot swallow a curved band piece by piece.
      const int ix0 = std::max(o.x, r.x);
      const int iy0 = std::max(o.y, r.y);
      const int ix1 = std::min(o.x + o.w, r.x + r.w);
      const int iy1 = std::min(o.y + o.h, r.y + r.h);
      const int overlap = (ix1 > ix0 && iy1 > iy0) ? (ix1 - ix0) * (iy1 - iy0) : 0;
      const int waste = rectArea(u) - rectArea(o) - rectArea(r) + overlap;
      if (waste * 4 > std::min(rectArea(o), rectArea(r))) {
        continue;
      }
      r = u;
      list->rects[i] = list->rects[--list->count];
      merged = true;
      break;
    }
  }
  if (list->count < kMaxRegionRects) {
    list->rects[list->count++] = r;
    return;
  }
  // Full: grow the rect that grows least.
  int best = 0;
  int best_growth = INT_MAX;
  for (int i = 0; i < list->count; ++i) {
    const int growth = rectArea(unionRect(list->rects[i], r)) - rectArea(list->rects[i]);
    if (growth < best_growth) {
      best_growth = growth;
      best = i;
    }
  }
  list->rects[best] = unionRect(list->rects[best], r);
}

bool regionTouches(const RegionList& list, const IntRect& r) {
  if (list.full) {
    return true;
  }
  for (int i = 0; i < list.count; ++i) {
    if (rectsOverlap(list.rects[i], r)) {
      return true;
    }
  }
  return false;
}

/** Pending repaint, consumed by the next radar frame (~1 KB: kept out of internal RAM). */
FS_BIG_STATIC(RegionList, s_dirty)
#define s_dirty FS_BIG_STATIC_REF(s_dirty)

// --- Region rebuild: clip by save / restore ---------------------------------------
// Every item is drawn whole, then the pixels it touched outside the region are put
// back as they were. That clips all writers alike (canvas primitives, AA text that
// reads the buffer, the sign's in-place darkening, direct buffer writes) without a
// clip rect in the drawing layer, and nothing outside the region is blended twice.

uint16_t* s_save_buf = nullptr;
size_t s_save_cap = 0;

bool ensureSaveBuf(size_t pixels) {
  if (s_save_buf != nullptr && s_save_cap >= pixels) {
    return true;
  }
  if (s_save_buf != nullptr) {
    heap_caps_free(s_save_buf);
    s_save_buf = nullptr;
    s_save_cap = 0;
  }
  // Round up so a slightly bigger item does not reallocate.
  const size_t want = std::max<size_t>(pixels, 64 * 1024);
  s_save_buf = static_cast<uint16_t*>(
      heap_caps_malloc(want * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_save_buf == nullptr) {
    return false;
  }
  s_save_cap = want;
  return true;
}

/** Non-null while a region rebuild draws: items outside it are skipped or clipped. */
const RegionList* s_paint_region = nullptr;
bool s_paint_failed = false;

bool saveContentRect(const IntRect& b) {
  if (!ensureSaveBuf(static_cast<size_t>(b.w) * static_cast<size_t>(b.h))) {
    return false;
  }
  const uint16_t* content = s_content.buffer();
  const int stride = s_content.width();
  for (int row = 0; row < b.h; ++row) {
    memcpy(s_save_buf + static_cast<size_t>(row) * b.w,
           content + static_cast<size_t>(b.y + row) * stride + b.x,
           static_cast<size_t>(b.w) * sizeof(uint16_t));
  }
  return true;
}

/** Put back every pixel of `b` that lies outside the region. */
void restoreContentOutside(const IntRect& b, const RegionList& region) {
  uint16_t* content = s_content.bufferMut();
  const int stride = s_content.width();
  int16_t spans[kMaxRegionRects][2];
  for (int row = 0; row < b.h; ++row) {
    const int y = b.y + row;
    // Region coverage on this row, clipped to b, sorted and merged.
    int n = 0;
    for (int i = 0; i < region.count; ++i) {
      const IntRect& r = region.rects[i];
      if (y < r.y || y >= r.y + r.h) {
        continue;
      }
      const int x0 = std::max(r.x, b.x);
      const int x1 = std::min(r.x + r.w, b.x + b.w);
      if (x1 <= x0) {
        continue;
      }
      int k = n++;
      while (k > 0 && spans[k - 1][0] > x0) {
        spans[k][0] = spans[k - 1][0];
        spans[k][1] = spans[k - 1][1];
        --k;
      }
      spans[k][0] = static_cast<int16_t>(x0);
      spans[k][1] = static_cast<int16_t>(x1);
    }
    uint16_t* dst = content + static_cast<size_t>(y) * stride;
    const uint16_t* src = s_save_buf + static_cast<size_t>(row) * b.w;
    auto put_back = [&](int from, int to) {
      if (to > from) {
        memcpy(dst + from, src + (from - b.x), static_cast<size_t>(to - from) * sizeof(uint16_t));
      }
    };
    int x = b.x;
    for (int i = 0; i < n; ++i) {
      put_back(x, spans[i][0]);
      x = std::max<int>(x, spans[i][1]);
    }
    put_back(x, b.x + b.w);
  }
}

/** Draw one item. Full rebuild: just draw. Region rebuild: skip it when it misses the
 *  region; otherwise draw it and undo whatever it painted outside the region. */
template <typename DrawFn>
void paintItem(const IntRect& bounds, DrawFn&& draw) {
  if (s_paint_region == nullptr) {
    draw();
    return;
  }
  const IntRect b = clampRectToScreen(bounds);
  if (rectEmpty(b) || !regionTouches(*s_paint_region, b)) {
    return;
  }
  for (int i = 0; i < s_paint_region->count; ++i) {
    if (rectContains(s_paint_region->rects[i], b)) {
      draw();  // nothing of it lands outside
      return;
    }
  }
  if (!saveContentRect(b)) {
    s_paint_failed = true;
    return;
  }
  draw();
  restoreContentOutside(b, *s_paint_region);
}

// --- Item bounds (conservative: every pixel the item can touch) --------------------

/** Half-side of a box that holds the full symbol at any heading. The icon is sc(28)
 *  square and rotates, so its corners reach ~sc(20); half + pad covers that. Some
 *  icons are drawn larger (the glider at 150%): never less than the farthest pixel
 *  the icon rasterizer can light for the largest of them. */
int symbolBoundHalfPx() {
  static const int half =
      std::max(aircraftSymbolHalfPx() + ui::sc(6), aircraft_symbol::fullReachPx());
  return half;
}

/** Same for the compact rim icon (its corners reach ~1.4x the half-side). */
int dotBoundRadiusPx() {
  static const int r = std::max({aircraft_symbol::radiusPx() / 2 + 4,
                                 aircraft_symbol::compactHalfPx() * 3 / 2 + 2,
                                 aircraft_symbol::compactReachPx()});
  return r;
}

IntRect symbolBounds(const PlaneView& v) {
  const int half = symbolBoundHalfPx();
  return IntRect(v.x - half, v.y - half, 2 * half + 1, 2 * half + 1);
}

IntRect dotBounds(const PlaneView& v) {
  const int r = dotBoundRadiusPx();
  return IntRect(v.x - r, v.y - r, 2 * r + 1, 2 * r + 1);
}

IntRect tagBounds(const PlaneView& v) {
  const int pad = ui::sc(6);
  return IntRect(v.tag_box.x - pad, v.tag_box.y - pad, v.tag_box.w + 2 * pad,
                 v.tag_box.h + 2 * pad);
}

uint16_t viewColor(const PlaneView& v, bool pulse_on) {
  if (!v.highlighted) {
    return v.base_color;
  }
  if (!pulse_on) {
    return radar::kColorGrid;
  }
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  if (v.notable && !v.emergency) {
    return tft.color565(255, 60, 220);  // notable aircraft pulse magenta
  }
#endif
  // With type colors the pulse keeps the class color so it stays recognizable.
  return v.emergency                            ? radar::kColorAlertEmergency
         : services::alert::typeColorsEnabled() ? v.base_color
                                                : radar::kColorAlertMilitary;
}

/**
 * Planes, tags and overlays from the cached views, in their fixed stacking order.
 * `update_sign`: let the overhead sign pick its plane (full rebuilds); region
 * rebuilds keep the sign as it is on screen.
 */
void drawAircraftLayer(bool update_sign) {
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  if (planes == nullptr) {
    return;
  }

  for (size_t d = 0; d < s_dot_order_count; ++d) {
    const PlaneView& v = s_views[s_dot_order[d]];
    paintItem(dotBounds(v), [&] {
      aircraft_symbol::drawCompact(*s_draw, v.x, v.y, v.disp_track, v.base_color,
                                   &planes[v.index]);
    });
  }

  const bool pulse_on = services::alert::pulsePhase();
  for (size_t d = 0; d < s_sym_order_count; ++d) {
    const PlaneView& v = s_views[s_sym_order[d]];
    paintItem(symbolBounds(v), [&] {
      aircraft_symbol::draw(*s_draw, v.x, v.y, v.disp_track, viewColor(v, pulse_on),
                            &planes[v.index]);
    });
  }
  if (!ui::displayPrefsHideBlipDetails()) {
    // Sorted farthest-first, so the closest tags draw last and land on top of
    // whatever residual overlap the resolver could not avoid.
    for (size_t d = 0; d < s_sym_order_count; ++d) {
      const PlaneView& v = s_views[s_sym_order[d]];
      if (v.tag_box.empty()) {
        continue;
      }
      paintItem(tagBounds(v), [&] { drawTagForView(v); });
    }
  }
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  // Over the planes and tags, under the link badge. The sign needs the target
  // buffer for its see-through backing; direct-to-panel fallbacks get a solid one.
  if (update_sign) {
    overheadSignUpdate();
  }
  {
    IntRect band;
    if (overheadSignBounds(&band.x, &band.y, &band.w, &band.h)) {
      paintItem(grownRect(band, 2), [&] {
        if (s_draw == &s_content.gfx()) {
          overheadSignDraw(*s_draw, s_content.bufferMut(), s_content.width(),
                           s_content.height());
        } else {
          overheadSignDraw(*s_draw, nullptr, 0, 0);
        }
      });
    }
  }
#else
  (void)update_sign;
#endif
  {
    IntRect badge;
    if (linkBadgeBox(&badge)) {
      paintItem(grownRect(badge, 2), [&] { drawLinkBadge(); });
    }
  }
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  {
    const IntRect mute = muteIconRect();
    if (s_paint_region == nullptr || regionTouches(*s_paint_region, mute)) {
      s_mute_icon_drawn = muteIconShown();
    }
    if (muteIconShown()) {
      paintItem(mute, [&] { drawMuteIcon(); });
    }
    const IntRect toast = soundToastRect();
    if (s_paint_region == nullptr || regionTouches(*s_paint_region, toast)) {
      s_sound_toast_drawn = s_sound_toast_until_ms != 0;
    }
    if (s_sound_toast_until_ms != 0) {
      paintItem(grownRect(toast, 1), [&] { drawSoundToast(); });
    }
  }
  {
    // Sky events: ISS marker, then the banner (top band).
    IntRect banner;
    IntRect marker;
    if (ui::sky::overlayDrawBounds(&banner.x, &banner.y, &banner.w, &banner.h, &marker.x,
                                   &marker.y, &marker.w, &marker.h)) {
      if (!rectEmpty(marker)) {
        paintItem(marker, [&] { ui::sky::drawRadarMarker(*s_draw); });
      }
      if (!rectEmpty(banner)) {
        paintItem(banner, [&] { ui::sky::drawRadarBanner(*s_draw); });
      }
    }
  }
#endif
  s_draw->setTextDatum(TextDatum::TopLeft);
}

/** Full aircraft layer onto the current target (refreshes the views first). */
void drawAircraft() {
  refreshPlaneViews();
  drawAircraftLayer(true);
}

// --- Background label mask -------------------------------------------------------
// One bit per pixel, set where a background label (compass letters, range labels,
// rain time) changed the background. The alert ring skips those pixels, so the
// labels stay on top of it.

constexpr size_t kLabelMaskStride = (static_cast<size_t>(radar::kSize) + 7) / 8;
uint8_t* s_label_mask = nullptr;
/** True while rebuildBackgroundSprite() draws its labels into the mask. */
bool s_label_mask_capture = false;

void clearLabelMask() {
  if (s_label_mask == nullptr) {
    s_label_mask = static_cast<uint8_t*>(heap_caps_malloc(
        kLabelMaskStride * radar::kSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (s_label_mask != nullptr) {
    memset(s_label_mask, 0, kLabelMaskStride * radar::kSize);
  }
}

/** Text box for a datum, from advance width and font height. */
IntRect textBoxForDatum(const char* text, int x, int y, TextDatum datum) {
  const int w = s_draw->textWidth(text);
  const int h = s_draw->fontHeight();
  int left = x;
  int top = y;
  switch (datum) {
    case TextDatum::TopCenter:
    case TextDatum::MiddleCenter:
    case TextDatum::BottomCenter:
      left = x - w / 2;
      break;
    case TextDatum::TopRight:
    case TextDatum::MiddleRight:
    case TextDatum::BottomRight:
      left = x - w;
      break;
    default:
      break;
  }
  switch (datum) {
    case TextDatum::MiddleLeft:
    case TextDatum::MiddleCenter:
    case TextDatum::MiddleRight:
      top = y - h / 2;
      break;
    case TextDatum::BottomLeft:
    case TextDatum::BottomCenter:
    case TextDatum::BottomRight:
      top = y - h;
      break;
    default:
      break;
  }
  return IntRect{left, top, w, h};
}

/** drawString for background labels; records the pixels it changed in the mask. */
void drawGridLabelString(const char* text, int x, int y, TextDatum datum) {
  s_draw->setTextDatum(datum);
  if (!s_label_mask_capture || s_label_mask == nullptr || !s_bg_ready ||
      s_draw != &s_bg.gfx()) {
    s_draw->drawString(text, x, y);
    return;
  }
  // Generous margin: the drawn glyphs follow their own bounds, not the advance box.
  const IntRect box = clampRectToScreen(grownRect(textBoxForDatum(text, x, y, datum), ui::sc(6)));
  if (rectEmpty(box) || !ensureSaveBuf(static_cast<size_t>(box.w) * box.h)) {
    s_draw->drawString(text, x, y);
    return;
  }
  const uint16_t* bg = s_bg.buffer();
  const int stride = s_bg.width();
  for (int row = 0; row < box.h; ++row) {
    memcpy(s_save_buf + static_cast<size_t>(row) * box.w,
           bg + static_cast<size_t>(box.y + row) * stride + box.x,
           static_cast<size_t>(box.w) * sizeof(uint16_t));
  }
  s_draw->drawString(text, x, y);
  for (int row = 0; row < box.h; ++row) {
    const int py = box.y + row;
    const uint16_t* now = bg + static_cast<size_t>(py) * stride;
    const uint16_t* was = s_save_buf + static_cast<size_t>(row) * box.w;
    uint8_t* mask = s_label_mask + static_cast<size_t>(py) * kLabelMaskStride;
    for (int col = 0; col < box.w; ++col) {
      const int px = box.x + col;
      if (now[px] != was[col]) {
        mask[px >> 3] = static_cast<uint8_t>(mask[px >> 3] | (1u << (px & 7)));
      }
    }
  }
}

void drawCardinalLabel(const char* text, int x, int y, TextDatum datum) {
  displayFontApply(*s_draw, s_cardinal_style);
  s_draw->setTextColor(radar::kColorGrid);
  drawGridLabelString(text, x, y, datum);
}

void drawScaleLabelWithBackground(const char* text, int x, int y) {
  displayFontApply(*s_draw, s_scale_style);
  // Keep the label's left end inside the round panel. Only moves labels that would
  // be clipped (e.g. the outer ring's "9.7km" on the 720 px panel); others are untouched.
  {
    const int w = s_draw->textWidth(text);
    const int h = s_scale_label_h;
    const int dy = y - h / 2 - radar::kCenterY;
    const int r = radar::kSize / 2 - ui::sc(4);
    if (dy * dy < r * r) {
      const int half_chord = static_cast<int>(sqrtf(static_cast<float>(r * r - dy * dy)));
      const int left_limit = radar::kCenterX - half_chord;
      if (x - w < left_limit) {
        x = left_limit + w;
      }
    }
  }
  // Bottom-right anchor: text grows up/left into the W–SW wedge (not toward S).
  s_draw->setTextColor(radar::kColorGrid);
  drawGridLabelString(text, x, y, TextDatum::BottomRight);
}

/** Convert degrees CW-from-top (12 o'clock) to Arduino_GFX fillArc degrees
 *  (0° = 3 o'clock, clockwise). */
float radarArcToGfxDeg(float deg_cw_from_top) {
  float a = deg_cw_from_top + 270.0f;
  while (a >= 360.0f) {
    a -= 360.0f;
  }
  while (a < 0.0f) {
    a += 360.0f;
  }
  return a;
}

void fillRadarArcDeg(int cx, int cy, int r_outer, int r_inner, float start_cw_from_top,
                     float end_cw_from_top, uint16_t color) {
  if (end_cw_from_top <= start_cw_from_top + 0.05f || r_outer <= 0) {
    return;
  }
  if (r_inner < 0) {
    r_inner = 0;
  }
  if (r_inner >= r_outer) {
    r_inner = r_outer - 1;
  }
  const float gs = radarArcToGfxDeg(start_cw_from_top);
  const float ge = radarArcToGfxDeg(end_cw_from_top);
  // fillArc fmods to [0,360); split when the sector crosses 3 o'clock.
  if (ge < gs) {
    s_draw->fillArc(static_cast<int16_t>(cx), static_cast<int16_t>(cy),
                    static_cast<int16_t>(r_outer), static_cast<int16_t>(r_inner), gs, 360.0f,
                    color);
    if (ge > 0.05f) {
      s_draw->fillArc(static_cast<int16_t>(cx), static_cast<int16_t>(cy),
                      static_cast<int16_t>(r_outer), static_cast<int16_t>(r_inner), 0.0f, ge,
                      color);
    }
  } else {
    s_draw->fillArc(static_cast<int16_t>(cx), static_cast<int16_t>(cy),
                    static_cast<int16_t>(r_outer), static_cast<int16_t>(r_inner), gs, ge,
                    color);
  }
}

void drawDashedWideLine(int x0, int y0, int x1, int y1, float half_width, uint16_t color) {
  const float dx = static_cast<float>(x1 - x0);
  const float dy = static_cast<float>(y1 - y0);
  const float len = sqrtf(dx * dx + dy * dy);
  if (len < 1.0f) {
    return;
  }

  const float ux = dx / len;
  const float uy = dy / len;
  const float dash = static_cast<float>(radar::kGridDashLenPx);
  const float gap = static_cast<float>(radar::kGridDashGapPx);
  const float period = dash + gap;

  for (float pos = 0.0f; pos + dash <= len; pos += period) {
    const float end = pos + dash;
    const int sx = x0 + static_cast<int>(lroundf(ux * pos));
    const int sy = y0 + static_cast<int>(lroundf(uy * pos));
    const int ex = x0 + static_cast<int>(lroundf(ux * end));
    const int ey = y0 + static_cast<int>(lroundf(uy * end));
    s_draw->drawWideLine(sx, sy, ex, ey, half_width, color);
  }
}

void drawDashedCircle(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }

  const int thickness =
      std::max(1, static_cast<int>(radar::kGridStrokeHalfWidth * 2.0f));
  const int r_outer = r;
  const int r_inner = std::max(0, r - thickness);

  constexpr float kRadToDeg = 180.0f / 3.14159265f;
  const float circumference = 2.0f * 3.14159265f * static_cast<float>(r);
  const float dash = static_cast<float>(radar::kGridDashLenPx);
  const float gap = static_cast<float>(radar::kGridDashGapPx);
  const float period = dash + gap;

  for (float arc_pos = 0.0f; arc_pos + dash <= circumference; arc_pos += period) {
    const float start_deg = (arc_pos / static_cast<float>(r)) * kRadToDeg;
    const float end_deg = ((arc_pos + dash) / static_cast<float>(r)) * kRadToDeg;
    fillRadarArcDeg(cx, cy, r_outer, r_inner, start_deg, end_deg, color);
  }
}

void drawGridRing(int cx, int cy, int r, uint16_t color) {
  if (r <= 0) {
    return;
  }
  drawDashedCircle(cx, cy, r, color);
}

void drawRings(int cx, int cy, int outer_radius) {
  for (int i = 1; i <= radar::kRingCount; ++i) {
    const int r = (outer_radius * i) / radar::kRingCount;
    drawGridRing(cx, cy, r, radar::kColorGrid);
  }
}

void drawGridSpokes(int cx, int cy, int radius, uint16_t color) {
  const float hw = radar::kGridStrokeHalfWidth;
  const float facing = static_cast<float>(radar::facingDeg());
  auto spoke = [&](float angle_deg_from_up) {
    const float rad = angle_deg_from_up * 3.14159265f / 180.0f;
    const int dx = static_cast<int>(lroundf(sinf(rad) * static_cast<float>(radius)));
    const int dy = static_cast<int>(lroundf(-cosf(rad) * static_cast<float>(radius)));
    drawDashedWideLine(cx - dx, cy - dy, cx + dx, cy + dy, hw, color);
  };
  // Geographic N-S / E-W / diagonals, rotated so facing is at screen-up.
  spoke(-facing);
  spoke(-facing + 90.0f);
  spoke(-facing + 45.0f);
  spoke(-facing + 135.0f);
}

void drawIntercardinalLabel(const char* text, int cx, int cy, int radius,
                            float angle_deg_from_north) {
  const float rad = angle_deg_from_north * 3.14159265f / 180.0f;
  const int r = radius - radar::kCardinalDiagonalInsetPx;
  const int x = cx + static_cast<int>(lroundf(sinf(rad) * static_cast<float>(r)));
  const int y = cy - static_cast<int>(lroundf(cosf(rad) * static_cast<float>(r)));
  drawCardinalLabel(text, x, y, TextDatum::MiddleCenter);
}

bool s_force_cardinals = false;

void drawCardinalLabels() {
  // Orientation preview always shows cardinals; otherwise respect compass rose.
  if (!s_force_cardinals) {
    if (!radar::showCompassRose()) {
      return;
    }
  }
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int rim_r = radar::kGridOuterRadius;
  const float facing = static_cast<float>(radar::facingDeg());

  // Geographic bearings → screen angle from up = bearing - facing.
  drawIntercardinalLabel("N", cx, cy, rim_r, 0.0f - facing);
  drawIntercardinalLabel("NE", cx, cy, rim_r, 45.0f - facing);
  drawIntercardinalLabel("E", cx, cy, rim_r, 90.0f - facing);
  drawIntercardinalLabel("SE", cx, cy, rim_r, 135.0f - facing);
  drawIntercardinalLabel("S", cx, cy, rim_r, 180.0f - facing);
  drawIntercardinalLabel("SW", cx, cy, rim_r, 225.0f - facing);
  drawIntercardinalLabel("W", cx, cy, rim_r, 270.0f - facing);
  drawIntercardinalLabel("NW", cx, cy, rim_r, 315.0f - facing);
}

/** Anchor scale text on the ring along kScaleLabelBearingDeg (between W and SW). */
void scaleLabelAnchorOnRing(int cx, int cy, int ring_radius, int gap_px, int* x,
                            int* y) {
  const float bearing =
      radar::kScaleLabelBearingDeg - static_cast<float>(radar::facingDeg());
  const float rad = bearing * 3.14159265f / 180.0f;
  const float r = static_cast<float>(ring_radius - gap_px);
  *x = cx + static_cast<int>(lroundf(sinf(rad) * r));
  *y = cy - static_cast<int>(lroundf(cosf(rad) * r));
}

void drawRingScaleLabels(int cx, int cy, int outer_radius) {
  const float label_km = radar::scaleActive().label_km;
  const radar::DistanceUnit unit = radar::distanceUnit();

  for (int ring = radar::kRingCount; ring >= 1; --ring) {
    const int ring_radius = (outer_radius * ring) / radar::kRingCount;
    const float ring_km =
        label_km * static_cast<float>(ring) / static_cast<float>(radar::kRingCount);

    char scale_label[12];
    radar::formatScaleTag(scale_label, sizeof(scale_label), ring_km, unit);

    int gap_px = radar::kScaleGapFromOuterRing;
    if (ring == radar::kRingCount && unit == radar::DistanceUnit::Km) {
      gap_px = radar::kScaleGapOuterRingKm;
    }

    int ax = 0;
    int ay = 0;
    scaleLabelAnchorOnRing(cx, cy, ring_radius, gap_px, &ax, &ay);
    drawScaleLabelWithBackground(scale_label, ax, ay);
  }
}

/** Rings / spokes / labels per the Rings setting; "off" leaves only a home marker. */
void drawGridForMode(int cx, int cy, int grid_r) {
  const radar::GridMode mode = radar::gridMode();
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  if (mode != radar::GridMode::Off) {
    drawRings(cx, cy, grid_r);
    drawGridSpokes(cx, cy, grid_r, radar::kColorGrid);
  }
  // "You are here" dot like a phone map: blue with a white rim, and a thin dark
  // edge so the rim still reads on white map areas.
  {
    const int r = ui::sc(7);
    const int rim = ui::sc(2);
    s_draw->fillCircle(cx, cy, r + 1, s_draw->color565(70, 70, 70));
    s_draw->fillCircle(cx, cy, r, s_draw->color565(255, 255, 255));
    s_draw->fillCircle(cx, cy, r - rim, s_draw->color565(26, 115, 232));
  }
#else
  if (mode != radar::GridMode::Off) {
    drawRings(cx, cy, grid_r);
    drawGridSpokes(cx, cy, grid_r, radar::kColorGrid);
  } else {
    // Small + at the radar center (home), two strokes thick.
    const int arm = ui::sc(9);
    const int t = ui::sc(2);
    s_draw->fillRect(cx - arm, cy - t / 2, 2 * arm + 1, t, radar::kColorGrid);
    s_draw->fillRect(cx - t / 2, cy - arm, t, 2 * arm + 1, radar::kColorGrid);
  }
#endif
  drawCardinalLabels();
  if (mode == radar::GridMode::Full) {
    drawRingScaleLabels(cx, cy, grid_r);
  }
}

template <typename GfxRef>
void drawStaticGrid(GfxRef& gfx) {
  initLabelMetrics();
  const DrawScope scope(gfx);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int grid_r = radar::kGridOuterRadius;

  gfx.fillScreen(radar::kColorBackground);
  drawGridForMode(cx, cy, grid_r);
  gfx.setTextDatum(TextDatum::TopLeft);
}

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
UiTextStyle rainLabelStyle() {
  static const UiTextStyle s_rain_style = pickTextSizeForHeight(ui::sc(6), 0, 2);
  return s_rain_style;
}

int rainLabelRightX() { return radar::kSize - ui::sc(10); }
int rainLabelMidY() { return radar::kCenterY + ui::sc(22); }

/** Small "Rain 9:05 AM" at the right edge, just below the E cardinal's spot. */
void drawRainTimestamp() {
  char label[24];
  if (!services::rain::labelText(label, sizeof(label))) {
    return;
  }
  displayFontApply(*s_draw, rainLabelStyle());
  s_draw->setTextColor(radar::kColorGrid);
  drawGridLabelString(label, rainLabelRightX(), rainLabelMidY(), TextDatum::MiddleRight);
}

/** Where the rain time sits (plus a margin), so tags keep off it. */
bool rainLabelRect(IntRect* out) {
  char label[24];
  if (!services::rain::labelText(label, sizeof(label))) {
    return false;
  }
  displayFontApply(tft, rainLabelStyle());
  const int w = tft.textWidth(label);
  const int h = tft.fontHeight();
  *out = grownRect(IntRect{rainLabelRightX() - w, rainLabelMidY() - h / 2, w, h}, ui::sc(3));
  return true;
}
#endif

bool rebuildBackgroundSprite() {
  const unsigned long t0 = millis();
  if (!s_bg_ready) {
    if (!s_bg.createSprite(radar::kSize, radar::kSize)) {
      Serial.println("radar: background sprite alloc failed");
      return false;
    }
    s_bg_ready = true;
  }

  // The labels drawn below record their pixels for the alert ring to keep clear of.
  clearLabelMask();
  s_label_mask_capture = true;

  // Optional Carto/OSM bake under the CRT grid (LittleFS JPEG → PSRAM cache).
  uint16_t* pixels = s_bg.bufferMut();
  const bool painted_map =
      pixels != nullptr &&
      services::basemap::blitRgb565(pixels, radar::kSize, radar::kSize);
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  // Live rain sits between the map (or plain background) and the grid/planes.
  if (!painted_map && pixels != nullptr) {
    s_bg.gfx().fillScreen(radar::kColorBackground);
  }
  const bool painted_rain =
      pixels != nullptr && services::rain::blendInto(pixels, radar::kSize, radar::kSize);
#else
  const bool painted_rain = false;
#endif
  if (painted_map || painted_rain) {
    initLabelMetrics();
    const DrawScope scope(s_bg.gfx());
    const int cx = radar::kCenterX;
    const int cy = radar::kCenterY;
    const int grid_r = radar::kGridOuterRadius;
    drawGridForMode(cx, cy, grid_r);
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    drawRainTimestamp();
#endif
    s_bg.gfx().setTextDatum(TextDatum::TopLeft);
  }
  if (painted_map) {
    s_basemap_bg_stale = false;
  } else {
    if (!painted_rain) {
      drawStaticGrid(s_bg.gfx());
    }
    // Keep stale if a map is expected but decode was deferred (HTTPS/heap).
    if (!services::basemap::wantsDisplay()) {
      s_basemap_bg_stale = false;
    }
  }
  s_label_mask_capture = false;
  s_content_base_valid = false;
  Serial.printf("[basemap] rebuild_bg painted=%d stale=%d cache=%d ms=%lu heap=%u\n",
                painted_map ? 1 : 0, s_basemap_bg_stale ? 1 : 0,
                services::basemap::cacheReady() ? 1 : 0, millis() - t0, ESP.getFreeHeap());
  return true;
}

bool ensureContentSprite() {
  if (s_content_ready) {
    return true;
  }
  if (!s_content.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: content sprite alloc failed");
    return false;
  }
  s_content_ready = true;
  return true;
}

void drawSweepAtOn(PlaneGfx& gfx, float lead_deg);
float currentSweepAngleDeg();
void resetDisplaySweepAngle(unsigned long now);
float advanceDisplaySweepAngle(unsigned long now);
void recoverSweepAfterGap(unsigned long gap_ms);
void savePrevAircraftMarkers();

void sweepSpokeEndpoint(float angle_deg, int* ex, int* ey) {
  constexpr float kDegToRad = 0.01745329252f;
  const float rad = angle_deg * kDegToRad;
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int r = radar::kSweepRadiusPx;
  *ex = cx + static_cast<int>(lroundf(sinf(rad) * static_cast<float>(r)));
  *ey = cy - static_cast<int>(lroundf(cosf(rad) * static_cast<float>(r)));
}

void copyBgRegionToContent(const IntRect& rect) {
  const IntRect clipped = clampRectToScreen(rect);
  if (rectEmpty(clipped)) {
    return;
  }

  const uint16_t* bg = s_bg.buffer();
  uint16_t* content = s_content.bufferMut();
  const int stride = s_bg.width();
  for (int y = clipped.y; y < clipped.y + clipped.h; ++y) {
    memcpy(content + static_cast<size_t>(y) * static_cast<size_t>(stride) +
               static_cast<size_t>(clipped.x),
           bg + static_cast<size_t>(y) * static_cast<size_t>(stride) +
               static_cast<size_t>(clipped.x),
           static_cast<size_t>(clipped.w) * sizeof(uint16_t));
  }
}

int collectSweepAngles(float lead_deg, float* angles, int max_angles) {
  if (max_angles <= 0) {
    return 0;
  }
  if (radar::kSweepTrailLines <= 1) {
    angles[0] = lead_deg;
    return 1;
  }

  int count = 0;
  for (int i = radar::kSweepTrailLines - 1; i >= 1; --i) {
    if (count >= max_angles - 1) {
      break;
    }
    const float t = static_cast<float>(i) / static_cast<float>(radar::kSweepTrailLines - 1);
    angles[count++] = lead_deg - t * radar::kSweepTrailSpanDeg;
  }
  if (count < max_angles) {
    angles[count++] = lead_deg;
  }
  return count;
}

// --- [perf] once a minute: rebuild / blit / sweep cost on the device --------------

struct PerfStat {
  uint32_t n = 0;
  uint64_t total_us = 0;
  uint32_t max_us = 0;

  void add(uint32_t us) {
    ++n;
    total_us += us;
    if (us > max_us) {
      max_us = us;
    }
  }
  double avgMs() const { return n == 0 ? 0.0 : static_cast<double>(total_us) / n / 1000.0; }
  double maxMs() const { return static_cast<double>(max_us) / 1000.0; }
};

PerfStat s_perf_full;
PerfStat s_perf_region;
PerfStat s_perf_blit;
PerfStat s_perf_sweep;
uint64_t s_perf_blit_px = 0;
unsigned long s_perf_window_ms = 0;
constexpr unsigned long kPerfLogMs = 60000;

void perfTick() {
  const unsigned long now = millis();
  if (s_perf_window_ms == 0) {
    s_perf_window_ms = now;
    return;
  }
  if (now - s_perf_window_ms < kPerfLogMs) {
    return;
  }
  Serial.printf(
      "[perf] radar %lus: full n=%u avg=%.1f max=%.1f ms | region n=%u avg=%.1f max=%.1f ms | "
      "blit n=%u avg=%.1f max=%.1f ms %luk px | sweep n=%u avg=%.2f max=%.1f ms\n",
      (now - s_perf_window_ms) / 1000UL, static_cast<unsigned>(s_perf_full.n),
      s_perf_full.avgMs(), s_perf_full.maxMs(), static_cast<unsigned>(s_perf_region.n),
      s_perf_region.avgMs(), s_perf_region.maxMs(), static_cast<unsigned>(s_perf_blit.n),
      s_perf_blit.avgMs(), s_perf_blit.maxMs(),
      static_cast<unsigned long>(s_perf_blit_px / 1000ULL), static_cast<unsigned>(s_perf_sweep.n),
      s_perf_sweep.avgMs(), s_perf_sweep.maxMs());
  s_perf_full = PerfStat{};
  s_perf_region = PerfStat{};
  s_perf_blit = PerfStat{};
  s_perf_sweep = PerfStat{};
  s_perf_blit_px = 0;
  s_perf_window_ms = now;
}

// --- Content rebuilds ---------------------------------------------------------------

void paintAlertRing(const IntRect& r) {
  if (alertFlashActive()) {
    alertFlashPaintBufferRect(s_content.bufferMut(), s_content.width(), s_content.height(), r.x,
                              r.y, r.w, r.h, s_label_mask);
  }
}

/** Background, alert ring, then every plane, tag and overlay; whole screen. */
bool rebuildContentBase(bool refresh_views) {
  if (!s_bg_ready || !ensureContentSprite()) {
    return false;
  }
  const uint32_t t0 = micros();

  const int w = s_bg.width();
  const int h = s_bg.height();
  const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  memcpy(s_content.bufferMut(), s_bg.buffer(), pixels * sizeof(uint16_t));
  // Under everything else, so tags, the banner, the sign and the edge labels stay
  // readable on top of it.
  paintAlertRing(IntRect{0, 0, w, h});

  {
    const DrawScope scope(s_content.gfx());
    if (refresh_views) {
      refreshPlaneViews();
    }
    drawAircraftLayer(true);
  }

  s_content_base_valid = true;
  s_perf_full.add(micros() - t0);
  return true;
}

#if defined(FLIGHTSCNR_SIM)
/** Simulator only: compare every region rebuild with a full one ([selfcheck] lines). */
constexpr bool kRegionSelfCheck = false;
#endif

/** Larger than this, a region rebuild costs about what a full one does. */
constexpr int kRegionMaxPixels = radar::kSize * radar::kSize * 45 / 100;

/**
 * Rebuild only `region` of the content sprite from the cached views. False when it
 * cannot (no valid content, stale views, no save buffer); the caller then does a
 * full rebuild.
 */
bool rebuildContentRegion(const RegionList& region) {
  if (!s_bg_ready || !s_content_ready || !s_content_base_valid || region.full ||
      region.count == 0 || region.area() > kRegionMaxPixels || !planeViewsCurrent()) {
    return false;
  }
  const uint32_t t0 = micros();
  for (int i = 0; i < region.count; ++i) {
    copyBgRegionToContent(region.rects[i]);
    paintAlertRing(region.rects[i]);
  }
  s_paint_region = &region;
  s_paint_failed = false;
  {
    const DrawScope scope(s_content.gfx());
    drawAircraftLayer(false);
  }
  s_paint_region = nullptr;
  if (s_paint_failed) {
    s_content_base_valid = false;  // partly drawn: only a full rebuild can fix it
    return false;
  }
  s_perf_region.add(micros() - t0);
#if defined(FLIGHTSCNR_SIM)
  if (kRegionSelfCheck) {
    // A region rebuild must match a full rebuild pixel for pixel.
    static uint16_t* region_px = nullptr;
    const size_t px = static_cast<size_t>(radar::kSize) * radar::kSize;
    if (region_px == nullptr) {
      region_px = static_cast<uint16_t*>(malloc(px * sizeof(uint16_t)));
    }
    memcpy(region_px, s_content.buffer(), px * sizeof(uint16_t));
    rebuildContentBase(false);
    size_t diff = 0;
    for (size_t i = 0; i < px; ++i) {
      diff += region_px[i] != s_content.buffer()[i] ? 1 : 0;
    }
    Serial.printf("[selfcheck] region rects=%d px=%d diff=%u\n", region.count, region.area(),
                  static_cast<unsigned>(diff));
  }
#endif
  return true;
}

void blitRegionToPanel(const IntRect& rect) {
  if (rectEmpty(rect) || !s_content_ready) {
    return;
  }
  const IntRect clipped = clampRectToScreen(rect);
  if (rectEmpty(clipped)) {
    return;
  }

  const uint16_t* content = s_content.buffer();
  const int stride = s_content.width();
  tft.blitRegionFromBuffer(static_cast<int16_t>(clipped.x),
                           static_cast<int16_t>(clipped.y),
                           static_cast<int16_t>(clipped.w),
                           static_cast<int16_t>(clipped.h),
                           content + static_cast<size_t>(clipped.y) * static_cast<size_t>(stride) +
                               static_cast<size_t>(clipped.x),
                           static_cast<int16_t>(stride));
  s_perf_blit_px += static_cast<uint64_t>(clipped.w) * static_cast<uint64_t>(clipped.h);
}

/** Copy the region (or the whole frame) from the content sprite to the panel. */
void blitRegionsToPanel(const RegionList& region) {
  const uint32_t t0 = micros();
  const IntRect kFull{0, 0, radar::kSize, radar::kSize};
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  // The P4 framebuffer takes any rect at memory speed: always just the patches.
  const bool push_full = region.full;
#else
  // QSPI panels: past about a third of the screen one full push beats many windows.
  const bool push_full = region.full || region.area() >= radar::kSize * radar::kSize / 3;
#endif
  if (push_full) {
    blitRegionToPanel(kFull);
  } else {
    for (int i = 0; i < region.count; ++i) {
      blitRegionToPanel(region.rects[i]);
    }
  }
  tft.setTextDatum(TextDatum::TopLeft);
  s_perf_blit.add(micros() - t0);
}

/** Highlighted planes' symbols: what the alert pulse changes. */
void markHighlightedSymbolsDirty() {
  if (!s_views_valid) {
    return;
  }
  for (size_t s = 0; s < s_sym_order_count; ++s) {
    const PlaneView& v = s_views[s_sym_order[s]];
    if (v.highlighted) {
      regionAdd(&s_dirty, symbolBounds(v));
    }
  }
}

/** The alert ring's annulus as horizontal bands (two short rects per band at the
 *  sides, one wide rect at the top and bottom). */
void markAlertRingDirty() {
  int ro = 0;
  int ri = 0;
  alertFlashRingRadii(&ro, &ri);
  ro += 1;
  ri = std::max(0, ri - 1);
  const int cx = radar::kCenterX;
  const int cy = radar::kCenterY;
  const int band = ui::sc(13);
  for (int y0 = cy - ro; y0 <= cy + ro; y0 += band) {
    const int y1 = std::min(y0 + band, cy + ro + 1);  // exclusive
    const int da = y0 - cy;
    const int db = y1 - 1 - cy;
    const int dmin = (da <= 0 && db >= 0) ? 0 : std::min(std::abs(da), std::abs(db));
    const int dmax = std::max(std::abs(da), std::abs(db));
    if (dmin > ro) {
      continue;
    }
    const int xo = static_cast<int>(sqrtf(static_cast<float>(ro * ro - dmin * dmin))) + 2;
    if (dmax >= ri) {
      regionAdd(&s_dirty, IntRect{cx - xo, y0, 2 * xo + 1, y1 - y0});
      continue;
    }
    const int xi = std::max(0, static_cast<int>(sqrtf(static_cast<float>(ri * ri - dmax * dmax))) - 2);
    regionAdd(&s_dirty, IntRect{cx - xo, y0, xo - xi + 1, y1 - y0});
    regionAdd(&s_dirty, IntRect{cx + xi, y0, xo - xi + 1, y1 - y0});
  }
}

// --- Sweep line ----------------------------------------------------------------------
// Each frame composes the new spoke in a spare full-size buffer and blits only thin
// per-row spans that cover the old and the new spoke. Every panel pixel goes
// straight from its old value to its new one (no erase-then-draw flicker on the
// scanned DSI framebuffer), a few thousand pixels move per frame instead of the
// spoke's bounding box, and the spoke is cut where the bottom sign and the top
// banner sit so it never runs through their text.

PlaneGfxSprite s_sweep_buf(&tft);
bool s_sweep_buf_ready = false;
bool s_sweep_buf_failed = false;

constexpr int kMaxSpokes = 8;
float s_prev_spoke_deg[kMaxSpokes];
/** Spokes on the panel right now (0 = none drawn, nothing to erase). */
int s_prev_spoke_count = 0;

int sweepSpanMargin() {
  return static_cast<int>(radar::kSweepLineHalfWidth) + 3 +
         (hardware::panelUsesCo5300() ? 2 : 0);
}

bool ensureSweepBuf() {
  if (s_sweep_buf_ready) {
    return true;
  }
  if (s_sweep_buf_failed) {
    return false;
  }
  if (!s_sweep_buf.createSprite(radar::kSize, radar::kSize)) {
    Serial.println("radar: sweep buffer alloc failed (direct sweep drawing)");
    s_sweep_buf_failed = true;
    return false;
  }
  s_sweep_buf_ready = true;
  return true;
}

/** x-range [*x0, *x1) of row y within `m` px of the spoke center->(ex, ey). */
bool spokeRowSpan(int ex, int ey, int m, int y, int* x0, int* x1) {
  const float sx = static_cast<float>(radar::kCenterX);
  const float sy = static_cast<float>(radar::kCenterY);
  const float dx = static_cast<float>(ex) - sx;
  const float dy = static_cast<float>(ey) - sy;
  const float ylo = std::min(sy, static_cast<float>(ey)) - m;
  const float yhi = std::max(sy, static_cast<float>(ey)) + m;
  const float fy = static_cast<float>(y);
  if (fy < ylo || fy > yhi) {
    return false;
  }
  float ta = 0.0f;
  float tb = 1.0f;
  if (std::fabs(dy) >= 0.5f) {
    ta = (fy - m - sy) / dy;
    tb = (fy + m - sy) / dy;
    if (ta > tb) {
      std::swap(ta, tb);
    }
    ta = std::max(ta, 0.0f);
    tb = std::min(tb, 1.0f);
    if (ta > tb) {
      ta = tb = std::min(std::max(ta, 0.0f), 1.0f);
    }
  }
  const float xa = sx + ta * dx;
  const float xb = sx + tb * dx;
  int lo = static_cast<int>(floorf(std::min(xa, xb))) - m;
  int hi = static_cast<int>(ceilf(std::max(xa, xb))) + m + 1;
  lo = std::max(lo, 0);
  hi = std::min(hi, static_cast<int>(radar::kSize));
  if (hi <= lo) {
    return false;
  }
  *x0 = lo;
  *x1 = hi;
  return true;
}

/** Visit each row's merged spans covering the given spokes. */
template <typename Fn>
void forEachSpokeSpan(const int* ex, const int* ey, int n, Fn&& fn) {
  if (n <= 0) {
    return;
  }
  const int m = sweepSpanMargin();
  int ymin = radar::kCenterY;
  int ymax = radar::kCenterY;
  for (int i = 0; i < n; ++i) {
    ymin = std::min(ymin, ey[i]);
    ymax = std::max(ymax, ey[i]);
  }
  ymin = std::max(0, ymin - m);
  ymax = std::min(static_cast<int>(radar::kSize) - 1, ymax + m);
  int spans[2 * kMaxSpokes][2];
  for (int y = ymin; y <= ymax; ++y) {
    int count = 0;
    for (int i = 0; i < n; ++i) {
      int a = 0;
      int b = 0;
      if (!spokeRowSpan(ex[i], ey[i], m, y, &a, &b)) {
        continue;
      }
      int k = count++;
      while (k > 0 && spans[k - 1][0] > a) {
        spans[k][0] = spans[k - 1][0];
        spans[k][1] = spans[k - 1][1];
        --k;
      }
      spans[k][0] = a;
      spans[k][1] = b;
    }
    int cur0 = -1;
    int cur1 = -1;
    for (int i = 0; i < count; ++i) {
      if (cur0 >= 0 && spans[i][0] <= cur1) {
        cur1 = std::max(cur1, spans[i][1]);
        continue;
      }
      if (cur0 >= 0) {
        fn(y, cur0, cur1);
      }
      cur0 = spans[i][0];
      cur1 = spans[i][1];
    }
    if (cur0 >= 0) {
      fn(y, cur0, cur1);
    }
  }
}

/** Liang-Barsky: the part [*tin, *tout] of P0->P1 inside r, if any. */
bool segmentInRect(float x0, float y0, float x1, float y1, const IntRect& r, float* tin,
                   float* tout) {
  float t0 = 0.0f;
  float t1 = 1.0f;
  const float dx = x1 - x0;
  const float dy = y1 - y0;
  auto clip = [&](float p, float q) {
    if (std::fabs(p) < 1e-6f) {
      return q >= 0.0f;
    }
    const float t = q / p;
    if (p < 0.0f) {
      if (t > t1) return false;
      if (t > t0) t0 = t;
    } else {
      if (t < t0) return false;
      if (t < t1) t1 = t;
    }
    return true;
  };
  if (clip(-dx, x0 - static_cast<float>(r.x)) &&
      clip(dx, static_cast<float>(r.x + r.w) - x0) &&
      clip(-dy, y0 - static_cast<float>(r.y)) &&
      clip(dy, static_cast<float>(r.y + r.h) - y0)) {
    *tin = t0;
    *tout = t1;
    return true;
  }
  return false;
}

/**
 * How far along center->end (t in [0, 1]) the spoke may run: it stops short of the
 * bottom sign and of the top banner, so it never cuts through their text.
 */
float spokeVisibleEnd(int ex, int ey) {
  float end = 1.0f;
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  const float x0 = static_cast<float>(radar::kCenterX);
  const float y0 = static_cast<float>(radar::kCenterY);
  const float x1 = static_cast<float>(ex);
  const float y1 = static_cast<float>(ey);
  const int keep_off = static_cast<int>(radar::kSweepLineHalfWidth) + 3;
  IntRect band;
  if (overheadSignBounds(&band.x, &band.y, &band.w, &band.h)) {
    const float cut_y = static_cast<float>(band.y - keep_off);
    if (y1 > cut_y && cut_y > y0) {
      end = std::min(end, (cut_y - y0) / (y1 - y0));
    }
  }
  IntRect banner;
  if (ui::sky::bannerBounds(&banner.x, &banner.y, &banner.w, &banner.h)) {
    float tin = 0.0f;
    float tout = 0.0f;
    if (segmentInRect(x0, y0, x1, y1, grownRect(banner, keep_off), &tin, &tout)) {
      end = std::min(end, tin);
    }
  }
#else
  (void)ex;
  (void)ey;
#endif
  return end;
}

/** Spoke straight onto the panel (no compose buffer), cut like the composed one. */
void drawClippedSpoke(PlaneGfx& gfx, float angle_deg, uint16_t color) {
  int ex = 0;
  int ey = 0;
  sweepSpokeEndpoint(angle_deg, &ex, &ey);
  const float t = spokeVisibleEnd(ex, ey);
  if (t * static_cast<float>(radar::kSweepRadiusPx) < 2.0f) {
    return;
  }
  const float x0 = static_cast<float>(radar::kCenterX);
  const float y0 = static_cast<float>(radar::kCenterY);
  gfx.drawWideLine(radar::kCenterX, radar::kCenterY,
                   static_cast<int16_t>(lroundf(x0 + t * (static_cast<float>(ex) - x0))),
                   static_cast<int16_t>(lroundf(y0 + t * (static_cast<float>(ey) - y0))),
                   radar::kSweepLineHalfWidth, color);
}

void drawClippedSweep(PlaneGfx& gfx, const float* angles, int count) {
  // Trail first (older, dimmer), lead last.
  for (int i = 0; i < count; ++i) {
    const bool lead = i == count - 1;
    drawClippedSpoke(gfx, angles[i], lead ? radar::kColorSweep : radar::kColorSweepTrail);
  }
}

/**
 * Rasterize one spoke into `buf` by distance to its center line: an even width at
 * every angle (the generic wide line leaves a checkerboard on 45-degree diagonals).
 * Writes only inside the spoke's own spans, which the caller has just refreshed.
 */
void rasterSpoke(uint16_t* buf, int stride, float angle_deg, uint16_t color) {
  int ex = 0;
  int ey = 0;
  sweepSpokeEndpoint(angle_deg, &ex, &ey);
  const float t_end = spokeVisibleEnd(ex, ey);
  if (t_end * static_cast<float>(radar::kSweepRadiusPx) < 2.0f) {
    return;
  }
  const float ax = static_cast<float>(radar::kCenterX);
  const float ay = static_cast<float>(radar::kCenterY);
  const float dx = static_cast<float>(ex) - ax;
  const float dy = static_cast<float>(ey) - ay;
  const float len2 = dx * dx + dy * dy;
  const float hw = radar::kSweepLineHalfWidth + 0.3f;
  const float hw2 = hw * hw;
  const int one_ex[1] = {ex};
  const int one_ey[1] = {ey};
  forEachSpokeSpan(one_ex, one_ey, 1, [&](int y, int a, int b) {
    const float py = static_cast<float>(y) - ay;
    uint16_t* row = buf + static_cast<size_t>(y) * stride;
    for (int x = a; x < b; ++x) {
      const float px = static_cast<float>(x) - ax;
      float u = (px * dx + py * dy) / len2;
      u = std::min(std::max(u, 0.0f), t_end);
      const float qx = px - u * dx;
      const float qy = py - u * dy;
      if (qx * qx + qy * qy <= hw2) {
        row[x] = color;
      }
    }
  });
}

/** Put the content sprite back under the spokes now on the panel. */
void eraseSweepFromPanel() {
  if (s_prev_spoke_count == 0 || !s_content_ready || !s_content_base_valid) {
    s_prev_spoke_count = 0;
    return;
  }
  int ex[kMaxSpokes];
  int ey[kMaxSpokes];
  for (int i = 0; i < s_prev_spoke_count; ++i) {
    sweepSpokeEndpoint(s_prev_spoke_deg[i], &ex[i], &ey[i]);
  }
  const uint16_t* content = s_content.buffer();
  const int stride = s_content.width();
  forEachSpokeSpan(ex, ey, s_prev_spoke_count, [&](int y, int a, int b) {
    tft.blitRegionFromBuffer(static_cast<int16_t>(a), static_cast<int16_t>(y),
                             static_cast<int16_t>(b - a), 1,
                             content + static_cast<size_t>(y) * stride + a,
                             static_cast<int16_t>(stride));
  });
  s_prev_spoke_count = 0;
}

void updateSweepOnPanel(float lead_deg) {
  const uint32_t t0 = micros();
  float angles[kMaxSpokes] = {};
  const int count = collectSweepAngles(lead_deg, angles, kMaxSpokes);

  if (!ensureSweepBuf() || !s_content_base_valid) {
    // No spare buffer: erase the old spoke from the content sprite, draw on the panel.
    eraseSweepFromPanel();
    drawClippedSweep(tft, angles, count);
  } else {
    // Old and new spokes together: content -> compose buffer -> new spoke -> panel.
    int ex[2 * kMaxSpokes];
    int ey[2 * kMaxSpokes];
    int n = 0;
    for (int i = 0; i < s_prev_spoke_count; ++i, ++n) {
      sweepSpokeEndpoint(s_prev_spoke_deg[i], &ex[n], &ey[n]);
    }
    for (int i = 0; i < count; ++i, ++n) {
      sweepSpokeEndpoint(angles[i], &ex[n], &ey[n]);
    }
    const uint16_t* content = s_content.buffer();
    uint16_t* compose = s_sweep_buf.bufferMut();
    const int stride = s_content.width();
    forEachSpokeSpan(ex, ey, n, [&](int y, int a, int b) {
      const size_t o = static_cast<size_t>(y) * stride + a;
      memcpy(compose + o, content + o, static_cast<size_t>(b - a) * sizeof(uint16_t));
    });
    for (int i = 0; i < count; ++i) {  // trail first, lead last
      rasterSpoke(compose, stride, angles[i],
                  i == count - 1 ? radar::kColorSweep : radar::kColorSweepTrail);
    }
    forEachSpokeSpan(ex, ey, n, [&](int y, int a, int b) {
      tft.blitRegionFromBuffer(static_cast<int16_t>(a), static_cast<int16_t>(y),
                               static_cast<int16_t>(b - a), 1,
                               compose + static_cast<size_t>(y) * stride + a,
                               static_cast<int16_t>(stride));
    });
  }
  tft.setTextDatum(TextDatum::TopLeft);

  for (int i = 0; i < count; ++i) {
    s_prev_spoke_deg[i] = angles[i];
  }
  s_prev_spoke_count = count;
  const uint32_t us = micros() - t0;
  s_perf_sweep.add(us);
  if (config::kRadarSweepTraceDebug && us >= 8000) {
    Serial.printf("[sweep] panel ang=%.1f us=%lu\n", lead_deg, static_cast<unsigned long>(us));
  }
}

void drawSweepSpokeOn(PlaneGfx& gfx, float angle_deg, uint16_t color) {
  int ex = 0;
  int ey = 0;
  sweepSpokeEndpoint(angle_deg, &ex, &ey);
  gfx.drawWideLine(radar::kCenterX, radar::kCenterY, ex, ey, radar::kSweepLineHalfWidth,
                   color);
}

/** Unclipped spoke straight onto a target (no-sprite fallbacks only). */
void drawSweepAtOn(PlaneGfx& gfx, float lead_deg) {
  if (radar::kSweepTrailLines <= 1) {
    drawSweepSpokeOn(gfx, lead_deg, radar::kColorSweep);
    return;
  }

  for (int i = radar::kSweepTrailLines - 1; i >= 1; --i) {
    const float t = static_cast<float>(i) / static_cast<float>(radar::kSweepTrailLines - 1);
    const float angle = lead_deg - t * radar::kSweepTrailSpanDeg;
    drawSweepSpokeOn(gfx, angle, radar::kColorSweepTrail);
  }
  drawSweepSpokeOn(gfx, lead_deg, radar::kColorSweep);
}

float currentSweepAngleDeg() {
  const unsigned long t = millis() % radar::kSweepPeriodMs;
  return 360.0f * static_cast<float>(t) / static_cast<float>(radar::kSweepPeriodMs);
}

void resetDisplaySweepAngle(unsigned long now) {
  s_display_sweep_deg = currentSweepAngleDeg();
  s_last_sweep_paint_ms = now;
  s_display_sweep_init = true;
}

float advanceDisplaySweepAngle(unsigned long now) {
  if (!s_display_sweep_init) {
    resetDisplaySweepAngle(now);
    return s_display_sweep_deg;
  }

  unsigned long dt = now - s_last_sweep_paint_ms;
  s_last_sweep_paint_ms = now;
  if (dt == 0) {
    return s_display_sweep_deg;
  }

  if (dt > radar::kSweepGapPauseMs) {
    dt = radar::kSweepFrameMs;
  } else if (dt > radar::kSweepMaxStepMs) {
    dt = radar::kSweepMaxStepMs;
  }

  const float rate = 360.0f / static_cast<float>(radar::kSweepPeriodMs);
  s_display_sweep_deg += rate * static_cast<float>(dt);
  if (s_display_sweep_deg >= 360.0f) {
    s_display_sweep_deg = std::fmod(s_display_sweep_deg, 360.0f);
  }
  return s_display_sweep_deg;
}

void recoverSweepAfterGap(unsigned long gap_ms) {
  if (gap_ms <= radar::kSweepGapPauseMs) {
    return;
  }
  if (s_prev_spoke_count > 0) {
    eraseSweepFromPanel();
    if (config::kRadarSweepTraceDebug) {
      Serial.printf("[sweep] gap_recover gap=%lums\n", gap_ms);
    }
  }
}

/** Symbol plus the tag box it was drawn with; the box MUST be the resolved one
 *  rather than a re-derived guess, or old text ghosts on the panel. */
IntRect aircraftMarkerBounds(int x, int y, const label_layout::Box& tag) {
  const int symbol_half = symbolBoundHalfPx();
  constexpr int kPad = ui::sc(6);

  int min_x = x - symbol_half;
  int max_x = x + symbol_half;
  int min_y = y - symbol_half;
  int max_y = y + symbol_half;

  if (!tag.empty()) {
    min_x = std::min(min_x, tag.x - kPad);
    max_x = std::max(max_x, tag.x + tag.w + kPad);
    min_y = std::min(min_y, tag.y - kPad);
    max_y = std::max(max_y, tag.y + tag.h + kPad);
  }

  return clampRectToScreen(
      IntRect(min_x, min_y, max_x - min_x + 1, max_y - min_y + 1));
}

/** Markers from the current views (hidden planes have none). */
size_t collectAircraftMarkers(CachedAircraftMarker* markers, size_t max_markers) {
  if (markers == nullptr || max_markers == 0) {
    return 0;
  }
  const services::adsb::Aircraft* planes = services::adsb::aircraftList();
  size_t count = 0;
  for (size_t i = 0; i < s_view_count && count < max_markers && planes != nullptr; ++i) {
    const PlaneView& v = s_views[i];
    CachedAircraftMarker& marker = markers[count++];
    marker.plane = planes[v.index];
    marker.x = v.x;
    marker.y = v.y;
    marker.beyond_dot = v.beyond;
    marker.tag_box = v.beyond ? label_layout::Box{} : v.tag_box;
    marker.color = v.base_color;
    marker.look = static_cast<uint8_t>((v.highlighted ? 1 : 0) | (v.notable ? 2 : 0) |
                                       (v.emergency ? 4 : 0));
    marker.text_hash = tagTextHash(v.text);
  }
  return count;
}

IntRect markerBounds(const CachedAircraftMarker& marker) {
  if (marker.beyond_dot) {
    const int dot_r = dotBoundRadiusPx();
    return clampRectToScreen(IntRect(marker.x - dot_r, marker.y - dot_r, dot_r * 2 + 1,
                                     dot_r * 2 + 1));
  }
  return aircraftMarkerBounds(marker.x, marker.y, marker.tag_box);
}

bool aircraftIdentityMatch(const services::adsb::Aircraft& a,
                           const services::adsb::Aircraft& b) {
  if (a.callsign[0] != '\0' && b.callsign[0] != '\0') {
    return strcmp(a.callsign, b.callsign) == 0;
  }
  constexpr float kPosEps = 0.001f;
  return fabsf(a.lat - b.lat) < kPosEps && fabsf(a.lon - b.lon) < kPosEps;
}

bool markerVisualChanged(const CachedAircraftMarker& prev,
                         const CachedAircraftMarker& curr) {
  if (prev.beyond_dot != curr.beyond_dot) {
    return true;
  }
  if (prev.x != curr.x || prev.y != curr.y) {
    return true;
  }
  // A tag can change slot or tier while its own aircraft sits still, when
  // traffic around it moves. Erasing only the old symbol would strand the text.
  if (!prev.tag_box.sameAs(curr.tag_box)) {
    return true;
  }
  if (fabsf(prev.plane.track_deg - curr.plane.track_deg) > 0.5f) {
    return true;
  }
  if (strcmp(prev.plane.callsign, curr.plane.callsign) != 0) {
    return true;
  }
  if (strcmp(prev.plane.type, curr.plane.type) != 0) {
    return true;
  }
  if (strcmp(prev.plane.alt, curr.plane.alt) != 0) {
    return true;
  }
  if (prev.plane.vert_rate_fpm != curr.plane.vert_rate_fpm) {
    return true;
  }
  if (prev.plane.db_flags != curr.plane.db_flags ||
      strcmp(prev.plane.squawk, curr.plane.squawk) != 0) {
    return true;  // highlight / color can follow these
  }
  // The icon follows the emitter category (often only on a later poll); watch-list
  // and notable matches can follow the registration or the hex.
  if (strcmp(prev.plane.category, curr.plane.category) != 0 ||
      strcmp(prev.plane.registration, curr.plane.registration) != 0 ||
      strcmp(prev.plane.hex, curr.plane.hex) != 0) {
    return true;
  }
  // What the views made of it all: color, highlight, notable, emergency, tag text.
  if (prev.color != curr.color || prev.look != curr.look || prev.text_hash != curr.text_hash) {
    return true;
  }
  return false;
}

/** Add the old and new bounds of every marker that moved or changed look. */
void addChangedMarkerBounds(const CachedAircraftMarker* current, size_t curr_count,
                            RegionList* out) {
  memset(s_marker_prev_used, 0, sizeof(s_marker_prev_used));

  for (size_t c = 0; c < curr_count; ++c) {
    int prev_i = -1;
    for (size_t p = 0; p < s_prev_aircraft_marker_count; ++p) {
      if (s_marker_prev_used[p]) {
        continue;
      }
      if (aircraftIdentityMatch(s_prev_aircraft_markers[p].plane, current[c].plane)) {
        prev_i = static_cast<int>(p);
        break;
      }
    }

    if (prev_i >= 0) {
      s_marker_prev_used[static_cast<size_t>(prev_i)] = true;
      if (markerVisualChanged(s_prev_aircraft_markers[static_cast<size_t>(prev_i)],
                              current[c])) {
        regionAdd(out, markerBounds(s_prev_aircraft_markers[static_cast<size_t>(prev_i)]));
        regionAdd(out, markerBounds(current[c]));
      }
    } else {
      regionAdd(out, markerBounds(current[c]));
    }
  }

  for (size_t p = 0; p < s_prev_aircraft_marker_count; ++p) {
    if (!s_marker_prev_used[p]) {
      regionAdd(out, markerBounds(s_prev_aircraft_markers[p]));
    }
  }
}

void savePrevAircraftMarkers() {
  s_prev_aircraft_marker_count =
      collectAircraftMarkers(s_prev_aircraft_markers, services::adsb::kMaxAircraft);
}

/**
 * Bring the content sprite and the panel up to date with s_dirty: a region rebuild
 * when only some rects changed (alert pulse, ring blink, banner, toast), else a full
 * one. False when nothing could be rebuilt (no sprites).
 */
bool rebuildAndBlitDirty() {
  bool full = s_dirty.full || !s_content_base_valid;
  if (!full && !rebuildContentRegion(s_dirty)) {
    full = true;
  }
  if (full) {
    if (!rebuildContentBase(true)) {
      return false;
    }
    // Views and the sign were refreshed: anything on screen may have moved.
    s_dirty.full = true;
  }
  blitRegionsToPanel(s_dirty);
  if (full) {
    savePrevAircraftMarkers();
  }
  s_dirty.clear();
  return true;
}

}  // namespace

static void blitStatic() {
  initPalette();

  if (!s_bg_ready) {
    const DrawScope scope(tft);
    drawStaticGrid(tft);
    drawAircraft();
    tft.setTextDatum(TextDatum::TopLeft);
    return;
  }

  if (!rebuildContentBase(true)) {
    s_bg.pushSprite(0, 0);
    drawAircraft();
    tft.setTextDatum(TextDatum::TopLeft);
    return;
  }

  s_content.pushSprite(0, 0);
  s_prev_spoke_count = 0;  // the full push replaced any spoke on the panel
  tft.setTextDatum(TextDatum::TopLeft);
  if (displayPrefsSweepLineEnabled()) {
    resetDisplaySweepAngle(millis());
    updateSweepOnPanel(s_display_sweep_deg);
  }
  savePrevAircraftMarkers();
}

void radarDisplayRefreshSweep() {
  radar::scaleTick();
  perfTick();
  initPalette();

  if (s_basemap_bg_stale) {
    const bool can_try =
        !services::basemap::wantsDisplay() || services::basemap::cacheReady() ||
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
        (ESP.getFreeHeap() >= 22000 &&  // 4C decodes into PSRAM; no HTTPS wait
#else
        (!services::https::busy() && ESP.getFreeHeap() >= 22000 &&
#endif
         ESP.getMaxAllocHeap() >= 10000);
    if (can_try) {
      Serial.printf("[basemap] applying stale bg https=%d heap=%u max_blk=%u\n",
                    services::https::busy() ? 1 : 0, ESP.getFreeHeap(),
                    ESP.getMaxAllocHeap());
      rebuildBackgroundSprite();
    } else {
      static unsigned long s_last_stale_log_ms = 0;
      const unsigned long now_ms = millis();
      if (now_ms - s_last_stale_log_ms >= 2000UL) {
        Serial.printf("[basemap] wait apply https=%d heap=%u max_blk=%u\n",
                      services::https::busy() ? 1 : 0, ESP.getFreeHeap(),
                      ESP.getMaxAllocHeap());
        s_last_stale_log_ms = now_ms;
      }
    }
  } else if (!s_bg_ready) {
    rebuildBackgroundSprite();
  }

  const unsigned long now = millis();
  const bool draw_sweep = displayPrefsSweepLineEnabled();

  if (!s_content_ready) {
    if (!ensureContentSprite()) {
      if (config::kRadarResumeDebug) {
        static unsigned long s_last_no_sprite_log_ms = 0;
        const unsigned long now_ms = millis();
        if (now_ms - s_last_no_sprite_log_ms >= 2000UL) {
          Serial.printf("[radar] sweep_no_sprite bg=%d content=%d base=%d heap=%u max_blk=%u\n",
                        s_bg_ready ? 1 : 0, s_content_ready ? 1 : 0,
                        s_content_base_valid ? 1 : 0, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          s_last_no_sprite_log_ms = now_ms;
        }
      }
      const DrawScope scope(tft);
      if (draw_sweep) {
        unsigned long paint_gap = 0;
        if (s_display_sweep_init && s_last_sweep_paint_ms > 0) {
          paint_gap = now - s_last_sweep_paint_ms;
        }
        recoverSweepAfterGap(paint_gap);
        const float angle = advanceDisplaySweepAngle(now);
        drawSweepAtOn(tft, angle);
      }
      drawAircraft();
      tft.setTextDatum(TextDatum::TopLeft);
      if (!s_dirty.empty()) {
        savePrevAircraftMarkers();
        s_dirty.clear();
      }
      return;
    }
  }

  // Alert pulse: only the highlighted symbols change color.
  {
    static bool s_last_pulse = false;
    const bool cur_pulse = services::alert::pulsePhase();
    if (cur_pulse != s_last_pulse) {
      s_last_pulse = cur_pulse;
      markHighlightedSymbolsDirty();
    }
  }
  // Alert ring blink (and its end): only the ring's band changes.
  {
    static uint32_t s_last_ring_phase = 0;
    const uint32_t phase = alertFlashPhase();
    if (phase != s_last_ring_phase) {
      s_last_ring_phase = phase;
      markAlertRingDirty();
    }
  }

  if (!s_dirty.empty() || !s_content_base_valid) {
    if (!rebuildAndBlitDirty()) {
      if (config::kRadarResumeDebug) {
        static unsigned long s_last_rebuild_fail_ms = 0;
        const unsigned long now_ms = millis();
        if (now_ms - s_last_rebuild_fail_ms >= 2000UL) {
          Serial.printf("[radar] sweep_rebuild_fail bg=%d content=%d base=%d dirty=%d heap=%u "
                        "max_blk=%u\n",
                        s_bg_ready ? 1 : 0, s_content_ready ? 1 : 0,
                        s_content_base_valid ? 1 : 0, s_dirty.empty() ? 0 : 1,
                        ESP.getFreeHeap(), ESP.getMaxAllocHeap());
          s_last_rebuild_fail_ms = now_ms;
        }
      }
      return;
    }
  }

  if (draw_sweep) {
    unsigned long paint_gap = 0;
    if (s_display_sweep_init && s_last_sweep_paint_ms > 0) {
      paint_gap = now - s_last_sweep_paint_ms;
    }
    recoverSweepAfterGap(paint_gap);
    const float angle = advanceDisplaySweepAngle(now);
    updateSweepOnPanel(angle);
  } else if (s_prev_spoke_count > 0) {
    eraseSweepFromPanel();
    tft.setTextDatum(TextDatum::TopLeft);
  }
}

void radarDisplayDraw() {
  initPalette();
  initLabelMetrics();
  s_dirty.clear();
  s_content_base_valid = false;
  s_prev_spoke_count = 0;
  s_display_sweep_init = false;
  s_prev_aircraft_marker_count = 0;

  if (rebuildBackgroundSprite() && ensureContentSprite()) {
    blitStatic();
    return;
  }

  const DrawScope scope(tft);
  drawStaticGrid(tft);
  if (displayPrefsSweepLineEnabled()) {
    resetDisplaySweepAngle(millis());
    drawSweepAtOn(tft, s_display_sweep_deg);
  }
  drawAircraft();
  tft.setTextDatum(TextDatum::TopLeft);
  savePrevAircraftMarkers();
}

void radarDisplayRefreshAircraft() {
  initPalette();
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  // First: whether the sign shows decides where tags may go.
  const bool sign_changed = overheadSignUpdate();
#endif
  // One pass per poll: positions, highlights, tag text and the new tag layout. The
  // dirty rects must cover where the labels are about to land, not where they were.
  refreshPlaneViews();

  const size_t curr_count = collectAircraftMarkers(s_current_aircraft_markers,
                                                    services::adsb::kMaxAircraft);
  // Pending rects (banner, toast...) ride along with this rebuild.
  addChangedMarkerBounds(s_current_aircraft_markers, curr_count, &s_dirty);
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  // Repaint the sign's band only when its text, logo or visibility changed.
  if (sign_changed) {
    IntRect band;
    overheadSignBounds(&band.x, &band.y, &band.w, &band.h);
    regionAdd(&s_dirty, band);
  }
#endif
  if (s_dirty.empty()) {
    return;
  }

  hardware::gfxLogf("[radar] adsb dirty rects=%d px=%d full=%d", s_dirty.count, s_dirty.area(),
                    s_dirty.full ? 1 : 0);
  if (config::kSerialTraceDebug) {
    Serial.printf("[radar] adsb dirty rects=%d px=%d full=%d\n", s_dirty.count, s_dirty.area(),
                  s_dirty.full ? 1 : 0);
  }

  if (!s_bg_ready) {
    rebuildBackgroundSprite();
  }

  // Apply the aircraft change to the panel immediately so blips track the ADS-B
  // poll (~2s) instead of waiting for a later frame. If the offscreen content sprite
  // is unavailable (low heap), the sweep path retries with a full rebuild.
  if (!ensureContentSprite() || !rebuildContentBase(false)) {
    s_content_base_valid = false;
    if (config::kRadarResumeDebug) {
      static unsigned long s_last_defer_ms = 0;
      const unsigned long now_ms = millis();
      if (now_ms - s_last_defer_ms >= 2000UL) {
        Serial.printf("[radar] ac_refresh_defer bg=%d content=%d base=%d heap=%u max_blk=%u\n",
                      s_bg_ready ? 1 : 0, s_content_ready ? 1 : 0,
                      s_content_base_valid ? 1 : 0, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
        s_last_defer_ms = now_ms;
      }
    }
    if (config::kRadarSweepTraceDebug) {
      Serial.println("[sweep] aircraft_refresh deferred (no content sprite)");
    }
    return;
  }

  PanelSession panel(tft);
  // A spoke cut by these blits is put right by the next sweep frame (it restores
  // the old spoke's pixels from the new content).
  blitRegionsToPanel(s_dirty);
  savePrevAircraftMarkers();
  s_dirty.clear();
}

void radarDisplaySetLinkStatus(RadarLinkStatus status) {
  if (status == s_link_status) {
    return;
  }
  s_link_status = status;
  // The badge blocks tags, so re-place them around it: repaint the whole layer.
  radarDisplayInvalidateAircraft();
}

RadarLinkStatus radarDisplayLinkStatus() { return s_link_status; }

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
namespace {
void markRadarRectDirty(const IntRect& r) { regionAdd(&s_dirty, r); }
}  // namespace

void radarDisplayShowSoundToast(bool sound_on) {
  s_sound_toast_on = sound_on;
  s_sound_toast_until_ms = millis() + kSoundToastMs;
  if (s_sound_toast_until_ms == 0) {
    s_sound_toast_until_ms = 1;
  }
  markRadarRectDirty(grownRect(soundToastRect(), 1));
  markRadarRectDirty(muteIconRect());
}

void radarDisplayTickSoundOverlay() {
  if (s_sound_toast_until_ms != 0 &&
      static_cast<long>(millis() - s_sound_toast_until_ms) >= 0) {
    s_sound_toast_until_ms = 0;
  }
  if (s_sound_toast_drawn != (s_sound_toast_until_ms != 0)) {
    markRadarRectDirty(grownRect(soundToastRect(), 1));
  }
  // Alert Sound can also change on settings page 3 or the web page.
  if (s_mute_icon_drawn != muteIconShown()) {
    markRadarRectDirty(muteIconRect());
  }
}
#endif
void radarDisplayInvalidateRect(int x, int y, int w, int h) {
  if (w <= 0 || h <= 0) {
    return;
  }
  regionAdd(&s_dirty, IntRect{x, y, w, h});
}

size_t radarDisplayInRangeAircraftCount() { return inRangeAircraftCount(); }

size_t radarDisplayVisibleAircraftCount() { return visibleAircraftCount(); }

bool radarDisplayIsInRange(const services::adsb::Aircraft& ac) { return isAircraftInRange(ac); }

void radarDisplayInvalidateAircraft() {
  s_dirty.full = true;
  s_content_base_valid = false;
  s_views_valid = false;
}

void radarDisplayInvalidateBasemap() {
  s_basemap_bg_stale = true;
  s_content_base_valid = false;
  Serial.println("[basemap] bg marked stale (will apply when HTTPS idle)");
}

void radarDisplayReleasePressureSprites() {
  s_prev_spoke_count = 0;
  if (s_sweep_buf_ready) {
    s_sweep_buf.deleteSprite();
    s_sweep_buf_ready = false;
  }
  if (s_save_buf != nullptr) {
    heap_caps_free(s_save_buf);
    s_save_buf = nullptr;
    s_save_cap = 0;
  }
  if (s_content_ready) {
    s_content.deleteSprite();
    s_content_ready = false;
    s_content_base_valid = false;
  }
  if (s_bg_ready) {
    s_bg.deleteSprite();
    s_bg_ready = false;
    s_content_base_valid = false;
  }
}

void drawOrientationHints(PlaneGfx& gfx) {
  char facing_buf[16];
  radar::facingLabel(facing_buf, sizeof(facing_buf));
  char line[40];
#if FLIGHTSCNR_HAS_ENCODER
  snprintf(line, sizeof(line), "Facing %s", facing_buf);
  const char* hint = "Tap to save";
#else
  // Touch only (4C): tap the left/right third to turn, the middle to save.
  snprintf(line, sizeof(line), "<  Facing %s  >", facing_buf);
  const char* hint = "Tap middle to save";
#endif

  // Body size matches settings rows; draw into a sprite/offscreen buffer so
  // CO5300 2x2 pixelAlign does not chunk the glyphs.
  displayFontApply(gfx, displayFontBody());
  gfx.setTextDatum(TextDatum::TopCenter);
  gfx.setTextColor(radar::kColorLabel, radar::kColorBackground);
  gfx.drawString(line, radar::kCenterX, ui::sc(28));
  // The touch hint is wider than "Tap to save"; keep it inside the round edge.
  gfx.drawString(hint, radar::kCenterX,
                 radar::kSize - (FLIGHTSCNR_HAS_ENCODER ? ui::sc(48) : ui::sc(72)));
  gfx.setTextDatum(TextDatum::TopLeft);
}

void radarDisplayDrawOrientationPreview() {
  initPalette();
  initLabelMetrics();
  s_force_cardinals = true;

  // Rebuild offscreen and push once — avoids the live fillScreen blank between
  // each 5° dial step that direct-to-panel redraw caused.
  if (rebuildBackgroundSprite()) {
    drawOrientationHints(s_bg.gfx());
    s_bg.pushSprite(0, 0);
  } else if (tft.beginOffscreen()) {
    {
      const DrawScope scope(tft);
      drawStaticGrid(tft);
      drawOrientationHints(tft);
    }
    tft.endOffscreen();
  } else {
    const DrawScope scope(tft);
    drawStaticGrid(tft);
    drawOrientationHints(tft);
  }

  s_force_cardinals = false;
}

bool radarDisplayDebugBgReady() { return s_bg_ready; }

bool radarDisplayDebugContentReady() { return s_content_ready; }

bool radarDisplayDebugContentBaseValid() { return s_content_base_valid; }

}  // namespace ui
