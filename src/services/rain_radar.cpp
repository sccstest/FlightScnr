#include "services/rain_radar.h"

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)

#include <Arduino.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <esp_heap_caps.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <new>

#include "services/adsb_client.h"
#include "services/clock_time.h"
#include "services/https_heap.h"
#include "services/https_lock.h"
#include "services/map_center.h"
#include "services/secure_client.h"
#include "ui/radar_display.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"

namespace services::rain {

namespace {

constexpr char kNs[] = "fs_rain";
constexpr char kEnKey[] = "en";

// NOAA NEXRAD n0q mosaic (0.5 dBZ steps, 5-minute cadence) from the Iowa
// Environmental Mesonet. Keyless, public domain; Web Mercator so it lines up
// with the basemap tiles.
constexpr char kWmsBase[] =
    "https://mesonet.agron.iastate.edu/cgi-bin/wms/nexrad/n0q.cgi?SERVICE=WMS&VERSION=1.1.1"
    "&REQUEST=GetMap&LAYERS=nexrad-n0q-900913&STYLES=&SRS=EPSG:3857&FORMAT=image/png"
    "&TRANSPARENT=true";
/** Sidecar with the mosaic's valid time, e.g. {"meta": {"valid": "2026-11-11T13:00:00Z", ...}} */
constexpr char kValidUrl[] =
    "https://mesonet.agron.iastate.edu/data/gis/images/4326/USCOMP/n0q_0.json";
constexpr char kUserAgent[] =
    "FlightScnr/" FLIGHTSCNR_FIRMWARE_VERSION
    " (rain overlay; +https://github.com/yashmulgaonkar/FlightScnr)";

/** WMS image edge; at the 20 mi range that is about the mosaic's native ~500 m
 *  cells across the panel's diagonal. Must stay <= 314: PNGdec's default line
 *  buffer ((320*4+1)*2 bytes) also holds 16-byte alignment padding for its two
 *  RGBA rows, and a 320 px row overruns it into the decoder's state. */
constexpr int kImg = 300;
static_assert(2 * (kImg * 4 + 1) + 45 <= (320 * 4 + 1) * 2, "PNGdec line buffer");
constexpr int kPanel = config::kDisplayWidth;
constexpr size_t kPanelPx = static_cast<size_t>(kPanel) * static_cast<size_t>(kPanel);
constexpr size_t kMaxPngBytes = 384 * 1024;
constexpr size_t kMaxJsonBytes = 2048;
constexpr uint32_t kTimeoutMs = 12000;

constexpr unsigned long kRefreshMs = 5UL * 60UL * 1000UL;   // mosaic cadence
constexpr unsigned long kViewChangeMinMs = 10000UL;       // after zoom/center/facing change
constexpr unsigned long kRetryMs = 60000UL;               // after a failed fetch
constexpr int64_t kStaleSec = 30 * 60;                    // hide frames older than this
constexpr unsigned long kStaleNoClockMs = 30UL * 60UL * 1000UL;
constexpr time_t kMinValidEpoch = 1600000000;

constexpr double kEarthRadiusKm = 6371.0088;      // same as tools/sim_bake_basemap.py
constexpr double kMercatorRadiusM = 6378137.0;    // EPSG:3857 sphere

// n0q color table (pyiem data/ramps/composite_n0q.txt): RGB -> dBZ x2.
struct RampEntry {
  uint8_t r, g, b;
  int16_t dbz_x2;
};
constexpr RampEntry kRamp[] = {
    // clang-format off
    {133, 113, 143, -64}, {133, 114, 143, -63}, {134, 115, 141, -62}, {135, 117, 139, -61},
    {135, 118, 139, -60}, {136, 119, 137, -59}, {137, 121, 135, -58}, {137, 122, 135, -57},
    {138, 123, 133, -56}, {139, 125, 132, -55}, {139, 126, 132, -54}, {140, 127, 130, -53},
    {141, 129, 128, -52}, {141, 130, 128, -51}, {142, 131, 126, -50}, {143, 132, 124, -49},
    {143, 133, 124, -48}, {144, 135, 123, -47}, {145, 136, 121, -46}, {145, 137, 121, -45},
    {146, 139, 119, -44}, {147, 141, 117, -43}, {150, 145, 83, -42}, {152, 148, 87, -41},
    {155, 151, 91, -40}, {157, 154, 96, -39}, {160, 157, 100, -38}, {163, 160, 104, -37},
    {165, 163, 109, -36}, {168, 166, 113, -35}, {170, 169, 118, -34}, {173, 172, 122, -33},
    {176, 175, 126, -32}, {178, 178, 131, -31}, {183, 184, 140, -30}, {186, 187, 144, -29},
    {189, 190, 148, -28}, {191, 193, 153, -27}, {194, 196, 157, -26}, {196, 199, 162, -25},
    {199, 202, 166, -24}, {202, 205, 170, -23}, {204, 208, 175, -22}, {210, 212, 180, -21},
    {207, 210, 180, -20}, {201, 204, 180, -19}, {198, 201, 180, -18}, {195, 199, 180, -17},
    {192, 196, 180, -16}, {189, 193, 180, -15}, {185, 190, 180, -14}, {182, 187, 180, -13},
    {179, 185, 180, -12}, {176, 182, 180, -11}, {173, 179, 180, -10}, {170, 176, 180, -9},
    {164, 171, 180, -8}, {160, 168, 180, -7}, {157, 165, 180, -6}, {154, 162, 180, -5},
    {151, 160, 180, -4}, {148, 157, 180, -3}, {145, 154, 180, -2}, {148, 155, 181, -1},
    {144, 152, 180, 0}, {140, 149, 179, 1}, {136, 146, 178, 2}, {128, 140, 176, 3},
    {124, 137, 175, 4}, {120, 134, 174, 5}, {116, 131, 172, 6}, {112, 128, 171, 7},
    {108, 125, 170, 8}, {103, 121, 169, 9}, {99, 118, 168, 10}, {95, 115, 167, 11},
    {91, 112, 166, 12}, {87, 109, 164, 13}, {79, 103, 162, 14}, {75, 100, 161, 15},
    {71, 97, 160, 16}, {67, 94, 159, 17}, {65, 91, 158, 18}, {67, 97, 162, 19},
    {69, 104, 166, 20}, {72, 111, 170, 21}, {74, 118, 174, 22}, {77, 125, 178, 23},
    {79, 132, 182, 24}, {81, 139, 187, 25}, {86, 153, 195, 26}, {89, 159, 199, 27},
    {91, 166, 203, 28}, {94, 173, 207, 29}, {96, 180, 212, 30}, {98, 187, 216, 31},
    {101, 194, 220, 32}, {103, 201, 224, 33}, {106, 208, 228, 34}, {111, 214, 232, 35},
    {104, 214, 215, 36}, {89, 214, 179, 37}, {82, 214, 162, 38}, {75, 214, 144, 39},
    {67, 214, 126, 40}, {60, 214, 109, 41}, {53, 214, 91, 42}, {17, 213, 24, 43},
    {17, 209, 23, 44}, {16, 205, 23, 45}, {16, 200, 22, 46}, {16, 196, 22, 47},
    {15, 188, 21, 48}, {15, 183, 20, 49}, {14, 179, 20, 50}, {14, 175, 19, 51},
    {14, 171, 19, 52}, {13, 166, 18, 53}, {13, 162, 18, 54}, {13, 158, 17, 55},
    {12, 153, 17, 56}, {12, 149, 16, 57}, {12, 145, 16, 58}, {11, 136, 15, 59},
    {11, 132, 14, 60}, {10, 128, 14, 61}, {10, 124, 13, 62}, {10, 119, 13, 63}, {9, 115, 12, 64},
    {9, 111, 12, 65}, {9, 107, 11, 66}, {8, 102, 11, 67}, {8, 98, 10, 68}, {9, 94, 9, 69},
    {50, 115, 8, 70}, {70, 125, 8, 71}, {91, 136, 7, 72}, {111, 146, 7, 73}, {132, 157, 6, 74},
    {152, 168, 6, 75}, {173, 178, 5, 76}, {193, 189, 5, 77}, {214, 199, 4, 78},
    {234, 210, 4, 79}, {255, 226, 0, 80}, {255, 216, 0, 81}, {255, 211, 0, 82},
    {255, 206, 0, 83}, {255, 201, 0, 84}, {255, 196, 0, 85}, {255, 192, 0, 86},
    {255, 187, 0, 87}, {255, 182, 0, 88}, {255, 177, 0, 89}, {255, 172, 0, 90},
    {255, 167, 0, 91}, {255, 162, 0, 92}, {255, 153, 0, 93}, {255, 148, 0, 94},
    {255, 143, 0, 95}, {255, 138, 0, 96}, {255, 133, 0, 97}, {255, 128, 0, 98}, {255, 0, 0, 99},
    {248, 0, 0, 100}, {241, 0, 0, 101}, {234, 0, 0, 102}, {227, 0, 0, 103}, {213, 0, 0, 104},
    {205, 0, 0, 105}, {198, 0, 0, 106}, {191, 0, 0, 107}, {184, 0, 0, 108}, {177, 0, 0, 109},
    {170, 0, 0, 110}, {163, 0, 0, 111}, {155, 0, 0, 112}, {148, 0, 0, 113}, {141, 0, 0, 114},
    {127, 0, 0, 115}, {120, 0, 0, 116}, {113, 0, 0, 117}, {255, 255, 255, 118},
    {255, 245, 255, 119}, {255, 234, 255, 120}, {255, 223, 255, 121}, {255, 212, 255, 122},
    {255, 201, 255, 123}, {255, 190, 255, 124}, {255, 179, 255, 125}, {255, 157, 255, 126},
    {255, 146, 255, 127}, {255, 117, 255, 128}, {252, 107, 253, 129}, {249, 96, 250, 130},
    {246, 86, 247, 131}, {243, 75, 244, 132}, {240, 64, 241, 133}, {237, 54, 239, 134},
    {234, 43, 236, 135}, {231, 32, 233, 136}, {225, 11, 227, 137}, {178, 0, 255, 138},
    {172, 0, 252, 139}, {164, 0, 247, 140}, {155, 0, 244, 141}, {147, 0, 239, 142},
    {136, 0, 234, 143}, {131, 0, 232, 144}, {121, 0, 226, 145}, {114, 0, 221, 146},
    {105, 0, 219, 147}, {5, 236, 240, 148}, {5, 235, 240, 149}, {5, 234, 240, 150},
    {5, 221, 224, 151}, {5, 220, 224, 152}, {5, 219, 224, 153}, {5, 205, 208, 154},
    {5, 204, 208, 155}, {4, 189, 192, 156}, {4, 188, 192, 157}, {4, 187, 192, 158},
    {4, 174, 176, 159}, {4, 173, 176, 160}, {4, 158, 160, 161}, {4, 157, 160, 162},
    {4, 156, 160, 163}, {3, 142, 144, 164}, {3, 141, 144, 165}, {3, 140, 144, 166},
    {3, 126, 128, 167}, {3, 125, 128, 168}, {3, 111, 112, 169}, {3, 110, 112, 170},
    {3, 109, 112, 171}, {2, 95, 96, 172}, {2, 94, 96, 173}, {2, 79, 80, 174}, {2, 78, 80, 175},
    {2, 77, 80, 176}, {2, 63, 64, 177}, {2, 62, 64, 178}, {2, 61, 64, 179}, {1, 48, 48, 180},
    {1, 47, 48, 181}, {1, 32, 32, 182}, {1, 31, 32, 183}, {1, 30, 32, 184}, {58, 103, 181, 185},
    {58, 102, 181, 186}, {58, 101, 181, 187}, {58, 100, 181, 188}, {58, 99, 181, 189},
    {58, 98, 181, 190},
    // clang-format on
};

/** Ignore echoes below light rain (clutter, drizzle) and the ramp's non-weather tail. */
constexpr int kMinDbzX2 = 36;   // 18 dBZ
constexpr int kMaxDbzX2 = 160;  // 80 dBZ

// Standard NWS reflectivity ramp, more opaque as the rain gets heavier so light
// showers tint the chart and storm cores read solid.
struct RainClass {
  int16_t min_dbz_x2;
  uint8_t r, g, b, a;
};
constexpr RainClass kClasses[] = {
    {36, 2, 253, 2, 90},      // 18 dBZ light rain
    {50, 1, 197, 1, 105},     // 25
    {60, 0, 142, 0, 120},     // 30
    {70, 253, 248, 2, 135},   // 35
    {80, 229, 188, 0, 145},   // 40
    {90, 253, 149, 0, 160},   // 45
    {100, 253, 0, 0, 175},    // 50 heavy
    {110, 212, 0, 0, 185},    // 55
    {120, 188, 0, 0, 190},    // 60
    {130, 248, 0, 253, 200},  // 65 hail / extreme
    {140, 152, 84, 198, 205}, // 70+
};
constexpr int kClassCount = sizeof(kClasses) / sizeof(kClasses[0]);
constexpr uint8_t kUnknown = 0xFF;

struct View {
  double lat = 0;
  double lon = 0;
  float label_km = 0;
  uint16_t facing = 0;
};

struct Payload {
  uint8_t* data = nullptr;
  size_t len = 0;
  size_t cap = 0;
  ~Payload() {
    if (data != nullptr) heap_caps_free(data);
  }
};

bool s_enabled = config::kFactoryRainOverlay;

// PSRAM buffers. s_live is read only by the loop; s_stage/s_img only by the
// worker while a job is pending; the loop swaps them once the job reports done.
uint8_t* s_live = nullptr;
uint8_t* s_stage = nullptr;
uint8_t* s_img = nullptr;          // decoded mosaic as dBZ x2 (0 = no echo)
uint8_t* s_color_cache = nullptr;  // RGB555 -> dBZ x2 (kUnknown until first seen)

bool s_has_live = false;
View s_live_view;
int64_t s_live_epoch = 0;
unsigned long s_live_fetched_ms = 0;
uint32_t s_frame_seq = 0;

View s_job_view;
int64_t s_job_epoch = 0;
volatile bool s_pending = false;
volatile bool s_ready = false;
volatile bool s_job_ok = false;
unsigned long s_last_attempt_ms = 0;
unsigned long s_retry_after_ms = 0;

bool s_drawn = false;
uint32_t s_drawn_seq = 0;
bool s_invalidate_sent = false;

void* psramAlloc(size_t bytes) {
  return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

bool ensureBuffers() {
  if (s_live == nullptr) s_live = static_cast<uint8_t*>(psramAlloc(kPanelPx));
  if (s_stage == nullptr) s_stage = static_cast<uint8_t*>(psramAlloc(kPanelPx));
  if (s_img == nullptr) s_img = static_cast<uint8_t*>(psramAlloc(kImg * kImg));
  if (s_color_cache == nullptr) {
    s_color_cache = static_cast<uint8_t*>(psramAlloc(32768));
    if (s_color_cache != nullptr) memset(s_color_cache, kUnknown, 32768);
  }
  return s_live != nullptr && s_stage != nullptr && s_img != nullptr &&
         s_color_cache != nullptr;
}

View currentView() {
  View v;
  v.lat = services::map_center::latitude();
  v.lon = services::map_center::longitude();
  v.label_km = ui::radar::scaleActive().label_km;
  v.facing = ui::radar::facingDeg();
  return v;
}

bool viewsMatch(const View& a, const View& b) {
  return fabs(a.lat - b.lat) < 0.0001 && fabs(a.lon - b.lon) < 0.0001 &&
         fabsf(a.label_km - b.label_km) < 0.01f && a.facing == b.facing;
}

bool liveFresh() {
  if (!s_has_live) return false;
  const time_t now = time(nullptr);
  if (s_live_epoch > 0 && now >= kMinValidEpoch) {
    return static_cast<int64_t>(now) - s_live_epoch <= kStaleSec;
  }
  return millis() - s_live_fetched_ms <= kStaleNoClockMs;
}

bool shouldShow() {
  return s_enabled && s_has_live && liveFresh() && viewsMatch(currentView(), s_live_view);
}

void requestRedraw() {
  if (s_invalidate_sent) return;
  s_invalidate_sent = true;
  ui::radarDisplayInvalidateBasemap();
}

void persistEnabled() {
  Preferences prefs;
  if (prefs.begin(kNs, false)) {
    prefs.putBool(kEnKey, s_enabled);
    prefs.end();
  }
}

// ---- worker side: fetch, decode, project ----------------------------------

int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

int64_t parseValidEpoch(const char* json) {
  const char* p = strstr(json, "\"valid\"");
  if (p == nullptr) return 0;
  p = strchr(p + 7, '"');
  if (p == nullptr) return 0;
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  if (sscanf(p + 1, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return 0;
  if (y < 2020 || mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
  return daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 +
         h * 3600 + mi * 60 + s;
}

bool payloadAppend(Payload* p, const uint8_t* src, size_t n, size_t max_bytes) {
  if (p->len + n + 1 > p->cap) {
    size_t grow = p->cap < 16384 ? 16384 : p->cap * 2;
    while (grow < p->len + n + 1) grow *= 2;
    if (grow > max_bytes + 1) grow = max_bytes + 1;
    if (p->len + n + 1 > grow) return false;
    auto* next = static_cast<uint8_t*>(
        heap_caps_realloc(p->data, grow, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (next == nullptr) return false;
    p->data = next;
    p->cap = grow;
  }
  memcpy(p->data + p->len, src, n);
  p->len += n;
  p->data[p->len] = 0;
  return true;
}

/** One HTTPS GET into a PSRAM buffer. HTTP/1.0 so the body is never chunked. */
bool httpGet(const String& url, Payload* out, size_t max_bytes) {
  services::https::SecureClient client;
  client.setInsecure();
  client.setTimeout(kTimeoutMs / 1000U);
  // Without this a stalled TLS handshake waits the library default (minutes) while
  // holding the shared fetch worker, and the ADS-B feed stops meanwhile.
  client.setHandshakeTimeout(kTimeoutMs / 1000U);
  HTTPClient http;
  http.setReuse(false);
  http.useHTTP10(true);
  http.setConnectTimeout(kTimeoutMs);
  http.setTimeout(kTimeoutMs);
  http.setUserAgent(kUserAgent);
  if (!http.begin(client, url)) {
    Serial.println("[rain] http.begin failed");
    return false;
  }
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("[rain] HTTP %d\n", code);
    http.end();
    return false;
  }
  WiFiClient* stream = http.getStreamPtr();
  bool ok = stream != nullptr;
  const unsigned long read_start = millis();
  uint8_t buf[1024];
  while (ok && (http.connected() || stream->available() > 0)) {
    if (millis() - read_start > kTimeoutMs) {  // wrap-safe
      Serial.println("[rain] read timeout");
      ok = false;
      break;
    }
    const int avail = stream->available();
    if (avail <= 0) {
      vTaskDelay(1);
      continue;
    }
    const size_t got = stream->readBytes(buf, avail < static_cast<int>(sizeof(buf))
                                                  ? static_cast<size_t>(avail)
                                                  : sizeof(buf));
    if (got > 0 && !payloadAppend(out, buf, got, max_bytes)) {
      Serial.println("[rain] payload too large / alloc failed");
      ok = false;
    }
  }
  http.end();
  client.stop();
  return ok && out->len > 0;
}

uint8_t classForDbzX2(int dbz_x2) {
  if (dbz_x2 < kMinDbzX2 || dbz_x2 > kMaxDbzX2) return 0;
  uint8_t cls = 0;
  for (int i = 0; i < kClassCount; ++i) {
    if (dbz_x2 >= kClasses[i].min_dbz_x2) cls = static_cast<uint8_t>(i + 1);
  }
  return cls;
}

/** Map a mosaic pixel color back to dBZ x2 via the n0q table (0 = not weather). */
uint8_t dbzForRgb(uint8_t r, uint8_t g, uint8_t b) {
  const uint16_t key = static_cast<uint16_t>(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
  uint8_t dbz = s_color_cache[key];
  if (dbz != kUnknown) return dbz;
  int best = -1;
  int best_d = 0x7fffffff;
  for (int i = 0; i < static_cast<int>(sizeof(kRamp) / sizeof(kRamp[0])); ++i) {
    const int dr = static_cast<int>(kRamp[i].r) - r;
    const int dg = static_cast<int>(kRamp[i].g) - g;
    const int db = static_cast<int>(kRamp[i].b) - b;
    const int dist = dr * dr + dg * dg + db * db;
    if (dist < best_d) {
      best_d = dist;
      best = i;
    }
  }
  // Far from every ramp color (map furniture, anti-aliasing): not rain.
  const int v = (best >= 0 && best_d <= 3 * 24 * 24) ? kRamp[best].dbz_x2 : 0;
  dbz = (v > 0 && v <= kMaxDbzX2) ? static_cast<uint8_t>(v) : 0;
  s_color_cache[key] = dbz;
  return dbz;
}

int pngDrawLine(PNGDRAW* draw) {
  if (draw == nullptr || draw->y < 0 || draw->y >= kImg) return 1;
  uint8_t* row = s_img + static_cast<size_t>(draw->y) * kImg;
  const int w = draw->iWidth < kImg ? draw->iWidth : kImg;
  const uint8_t* px = draw->pPixels;
  if (draw->iPixelType == PNG_PIXEL_TRUECOLOR_ALPHA && draw->iBpp == 8) {
    for (int x = 0; x < w; ++x, px += 4) {
      row[x] = px[3] < 128 ? 0 : dbzForRgb(px[0], px[1], px[2]);
    }
  } else if (draw->iPixelType == PNG_PIXEL_INDEXED && draw->iBpp == 8 &&
             draw->pPalette != nullptr) {
    const uint8_t* pal = draw->pPalette;
    for (int x = 0; x < w; ++x) {
      const uint8_t i = px[x];
      if (draw->iHasAlpha && pal[768 + i] < 128) {
        row[x] = 0;
      } else {
        row[x] = dbzForRgb(pal[i * 3], pal[i * 3 + 1], pal[i * 3 + 2]);
      }
    }
  } else {
    return 0;  // unexpected format: abort decode
  }
  return 1;
}

bool decodePng(uint8_t* data, size_t len) {
  void* mem = psramAlloc(sizeof(PNG));
  if (mem == nullptr) {
    Serial.println("[rain] PNG alloc failed");
    return false;
  }
  auto* png = new (mem) PNG();
  bool ok = false;
  if (png->openRAM(data, static_cast<int>(len), pngDrawLine) == PNG_SUCCESS) {
    if (png->getWidth() == kImg && png->getHeight() == kImg) {
      memset(s_img, 0, kImg * kImg);
      const int rc = png->decode(nullptr, 0);
      ok = rc == PNG_SUCCESS;
      if (!ok) {
        Serial.printf("[rain] png decode err=%d type=%d bpp=%d\n", rc, png->getPixelType(),
                      png->getBpp());
      }
    } else {
      Serial.printf("[rain] unexpected png %dx%d\n", png->getWidth(), png->getHeight());
    }
    png->close();
  } else {
    Serial.printf("[rain] png open failed err=%d\n", png->getLastError());
  }
  png->~PNG();
  heap_caps_free(mem);
  return ok;
}

/** Ground km from the radar center to the bbox edge: covers the panel corners. */
double coverKm(const View& v) {
  const double ppk = static_cast<double>(ui::radar::kGridOuterRadius) / v.label_km;
  return (std::hypot(kPanel / 2.0, kPanel / 2.0) + 2.0) / ppk;
}

/**
 * Panel pixel -> map, same as tools/sim_bake_basemap.py (screen up = facing,
 * flat-earth km offsets), then into the Web Mercator image. Mercator northing is
 * expanded to second order around the center latitude, so no per-pixel trig.
 */
void project(const View& v, uint8_t* out) {
  const double cover = coverKm(v);
  const float ppk = static_cast<float>(ui::radar::kGridOuterRadius) / v.label_km;
  const float f = static_cast<float>(v.facing) * static_cast<float>(M_PI) / 180.0f;
  const float cf = cosf(f);
  const float sf = sinf(f);
  const float k2 = static_cast<float>(0.5 * tan(v.lat * M_PI / 180.0) / kEarthRadiusKm);
  const float img_scale = static_cast<float>(kImg / (2.0 * cover));
  const float cov = static_cast<float>(cover);
  const float c = static_cast<float>(kPanel / 2);
  uint8_t cls_for[256];
  for (int i = 0; i < 256; ++i) cls_for[i] = classForDbzX2(i);
  auto at = [](int col, int row) -> int {
    if (col < 0 || col >= kImg || row < 0 || row >= kImg) return 0;
    return s_img[static_cast<size_t>(row) * kImg + col];
  };
  for (int py = 0; py < kPanel; ++py) {
    const float ny = (c - static_cast<float>(py)) / ppk;
    uint8_t* dst = out + static_cast<size_t>(py) * kPanel;
    for (int px = 0; px < kPanel; ++px) {
      const float ex = (static_cast<float>(px) - c) / ppk;
      const float e = ex * cf + ny * sf;
      const float n = -ex * sf + ny * cf;
      const float y = n + k2 * n * n;
      // Bilinear in dBZ (cell centers at +0.5) so contours come out smooth, not
      // as the mosaic's 500 m squares; then bucket into the color classes.
      const float fx = (e + cov) * img_scale - 0.5f;
      const float fy = (cov - y) * img_scale - 0.5f;
      const int x0 = static_cast<int>(floorf(fx));
      const int y0 = static_cast<int>(floorf(fy));
      if (x0 < -1 || x0 >= kImg || y0 < -1 || y0 >= kImg) {
        dst[px] = 0;
        continue;
      }
      const float wx = fx - static_cast<float>(x0);
      const float wy = fy - static_cast<float>(y0);
      const float top = at(x0, y0) + (at(x0 + 1, y0) - at(x0, y0)) * wx;
      const float bot = at(x0, y0 + 1) + (at(x0 + 1, y0 + 1) - at(x0, y0 + 1)) * wx;
      dst[px] = cls_for[static_cast<int>(top + (bot - top) * wy + 0.5f) & 0xFF];
    }
    if ((py & 63) == 63) vTaskDelay(1);
  }
}

String wmsUrl(const View& v) {
  const double cover = coverKm(v);
  const double lat_r = v.lat * M_PI / 180.0;
  const double half_m = kMercatorRadiusM * cover / (kEarthRadiusKm * cos(lat_r));
  const double mx = kMercatorRadiusM * v.lon * M_PI / 180.0;
  const double my = kMercatorRadiusM * log(tan(M_PI / 4.0 + lat_r / 2.0));
  char tail[160];
  snprintf(tail, sizeof(tail), "&WIDTH=%d&HEIGHT=%d&BBOX=%.1f,%.1f,%.1f,%.1f", kImg, kImg,
           mx - half_m, my - half_m, mx + half_m, my + half_m);
  String url = kWmsBase;
  url += tail;
  return url;
}

void rainJob() {
  const unsigned long t0 = millis();
  bool ok = false;
  int64_t epoch = 0;
  Payload png;
  {
    services::https::ScopedLock tls(kTimeoutMs * 2 + 4000);
    if (!tls.held()) {
      Serial.println("[rain] HTTPS busy (skipped)");
    } else {
      services::https::drainTlsHeapAfterSession();
      if (!services::https::heapReadyForPhoto()) {
        Serial.printf("[rain] defer: heap tight (free=%u max_blk=%u)\n",
                      static_cast<unsigned>(ESP.getFreeHeap()),
                      static_cast<unsigned>(ESP.getMaxAllocHeap()));
      } else {
        Payload meta;
        if (httpGet(kValidUrl, &meta, kMaxJsonBytes)) {
          epoch = parseValidEpoch(reinterpret_cast<const char*>(meta.data));
        }
        services::https::drainTlsHeapAfterSession();
        ok = httpGet(wmsUrl(s_job_view), &png, kMaxPngBytes);
      }
    }
  }
  services::https::drainTlsHeapAfterSession();
  if (ok) {
    ok = decodePng(png.data, png.len);
  }
  if (ok) {
    project(s_job_view, s_stage);
  }
  Serial.printf("[rain] job %s png=%uB valid=%lld ms=%lu\n", ok ? "ok" : "fail",
                static_cast<unsigned>(png.len), static_cast<long long>(epoch), millis() - t0);
  s_job_epoch = epoch;
  s_job_ok = ok;
  s_ready = true;
  s_pending = false;
}

}  // namespace

void init() {
  Preferences prefs;
  if (prefs.begin(kNs, true)) {
    s_enabled = prefs.getBool(kEnKey, config::kFactoryRainOverlay);
    prefs.end();
  }
}

bool enabled() { return s_enabled; }

void setEnabled(bool on) {
  if (on == s_enabled) return;
  s_enabled = on;
  s_retry_after_ms = 0;  // a back-off from before it was switched off is stale
  persistEnabled();
  requestRedraw();
}

void toggleEnabled() { setEnabled(!s_enabled); }

void saveEnabledFromForm(const char* v) {
  const bool on = v != nullptr && (strcmp(v, "T") == 0 || strcmp(v, "t") == 0 ||
                                   strcmp(v, "on") == 0 || strcmp(v, "true") == 0);
  setEnabled(on);
}

void tick(bool radar_visible, bool wifi_up) {
  const unsigned long now = millis();
  if (s_ready) {
    if (s_job_ok) {
      uint8_t* t = s_live;
      s_live = s_stage;
      s_stage = t;
      s_live_view = s_job_view;
      s_live_epoch = s_job_epoch;
      s_live_fetched_ms = now;
      s_has_live = true;
      ++s_frame_seq;
      s_retry_after_ms = 0;
    } else {
      s_retry_after_ms = now + kRetryMs;
    }
    s_ready = false;
  }

  // Appear, refresh, disappear (stale / toggled / view moved): rebuild once.
  const bool want = shouldShow();
  if (want != s_drawn || (want && s_drawn_seq != s_frame_seq)) {
    requestRedraw();
  }

  if (!s_enabled || !radar_visible || !wifi_up || s_pending || s_ready) return;
  // Signed difference: an absolute millis() deadline compared with < breaks when
  // millis() wraps (every 49.7 days of uptime).
  if (s_retry_after_ms != 0) {
    if (static_cast<long>(now - s_retry_after_ms) < 0) return;
    s_retry_after_ms = 0;  // passed: don't leave a stale deadline to wrap around later
  }
  const View view = currentView();
  const bool view_moved = s_has_live && !viewsMatch(view, s_live_view);
  const bool due = !s_has_live || now - s_live_fetched_ms >= kRefreshMs ||
                   (view_moved && now - s_last_attempt_ms >= kViewChangeMinMs);
  if (!due) return;
  if (!ensureBuffers()) {
    s_retry_after_ms = now + kRetryMs;
    Serial.println("[rain] PSRAM alloc failed");
    return;
  }
  s_job_view = view;
  s_pending = true;
  if (services::adsb::queueBackgroundJob(&rainJob)) {
    s_last_attempt_ms = now;
  } else {
    s_pending = false;  // worker busy; try again next loop
  }
}

bool blendInto(uint16_t* pixels, int w, int h) {
  s_invalidate_sent = false;
  s_drawn = false;
  if (pixels == nullptr || w != kPanel || h != kPanel || !shouldShow()) {
    return false;
  }
  uint16_t rgb[kClassCount + 1][3] = {};
  uint16_t alpha[kClassCount + 1] = {};
  for (int i = 0; i < kClassCount; ++i) {
    rgb[i + 1][0] = kClasses[i].r >> 3;
    rgb[i + 1][1] = kClasses[i].g >> 2;
    rgb[i + 1][2] = kClasses[i].b >> 3;
    alpha[i + 1] = kClasses[i].a;
  }
  const uint8_t* cls = s_live;
  for (size_t i = 0; i < kPanelPx; ++i) {
    const uint8_t c = cls[i];
    if (c == 0) continue;
    const uint16_t a = alpha[c];
    const uint16_t ia = 256 - a;
    const uint16_t p = pixels[i];
    const uint16_t r = static_cast<uint16_t>((((p >> 11) & 0x1F) * ia + rgb[c][0] * a) >> 8);
    const uint16_t g = static_cast<uint16_t>((((p >> 5) & 0x3F) * ia + rgb[c][1] * a) >> 8);
    const uint16_t b = static_cast<uint16_t>(((p & 0x1F) * ia + rgb[c][2] * a) >> 8);
    pixels[i] = static_cast<uint16_t>((r << 11) | (g << 5) | b);
  }
  s_drawn = true;
  s_drawn_seq = s_frame_seq;
  return true;
}

bool labelText(char* buf, size_t len) {
  if (buf == nullptr || len == 0 || !shouldShow() || s_live_epoch <= 0) return false;
  char hhmm[16];
  services::clock::formatClockFromEpoch(s_live_epoch, hhmm, sizeof(hhmm));
  snprintf(buf, len, "Rain %s", hhmm);
  return true;
}

}  // namespace services::rain

#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C
