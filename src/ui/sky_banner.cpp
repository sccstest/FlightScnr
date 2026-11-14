#include "ui/sky_banner.h"

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include <Arduino.h>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/clock_time.h"
#include "services/sky_events.h"
#include "ui/radar_display.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"
#include "ui/ui_scale.h"

namespace ui::sky {
namespace {

using services::sky::Banner;
using services::sky::EventKind;

// Top band of the round scope. The bottom band (overhead sign) and the left edge
// (mute icon) belong to other overlays.
constexpr int kBannerTopY = ui::sc(31);
constexpr int kPadX = ui::sc(14);
constexpr int kPadY = ui::sc(5);
constexpr int kLineGap = ui::sc(1);
constexpr int kDotR = ui::sc(2);
constexpr int kOutline = ui::sc(2);
/** The pill's outline stays this far inside the glass. */
constexpr int kGlassMargin = ui::sc(3);

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool empty() const { return w <= 0 || h <= 0; }
};

struct DrawState {
  Banner banner;
  Rect rect;
  bool marker = false;
  int mx = 0, my = 0;
  float m_el = 0;
};

DrawState s_state;       // what the radar frames draw
bool s_have_state = false;
unsigned long s_detail_last_ms = 0;
int64_t s_detail_last_sec = 0;

UiTextStyle titleStyle() { return displayFontScale(); }
UiTextStyle detailStyle() { return displayFontBody(); }

uint16_t accentFor(PlaneGfx& g, EventKind k) {
  return k == EventKind::Iss ? g.color565(90, 200, 255) : g.color565(255, 150, 40);
}

constexpr char kSepGap[] = "   ";  // where a '|' separator dot goes

/** The title with each '|' replaced by spaces (drawn as one string, so every part
 *  shares a baseline), and its pixel width. */
void spacedTitle(const char* title, char* out, size_t len) {
  size_t o = 0;
  for (const char* p = title; *p != 0 && o + 4 < len; ++p) {
    if (*p == '|') {
      memcpy(out + o, kSepGap, 3);
      o += 3;
    } else {
      out[o++] = *p;
    }
  }
  out[o] = 0;
}

int titleWidth(PlaneGfx& g, const char* title) {
  displayFontApply(g, titleStyle());
  char spaced[64];
  spacedTitle(title, spaced, sizeof(spaced));
  return g.textWidth(spaced);
}

/** Widest pill of height h whose round ends, outline included, stay on the glass. */
int maxBannerWidth(int h) {
  const int reach = config::kDisplayWidth / 2 - kGlassMargin - (h / 2 + kOutline);
  const int dy = radar::kCenterY - (kBannerTopY + h / 2);
  if (reach <= dy) return 3 * h;
  const int end_cx = static_cast<int>(std::sqrt(static_cast<float>(reach * reach - dy * dy)));
  return 2 * (end_cx + h / 2);
}

void drawTitle(PlaneGfx& g, const char* title, int cx, int y, int max_w, uint16_t color) {
  displayFontApply(g, titleStyle());
  char buf[48];
  snprintf(buf, sizeof(buf), "%s", title);
  // Too wide: drop trailing parts unless they are short (e.g. "Look east" /
  // "Liftoff!"), then shorten the first part, which holds the long names, with "...".
  char* bar = strrchr(buf, '|');
  while (bar != nullptr && titleWidth(g, buf) > max_w) {
    if (g.textWidth(bar + 1) < max_w / 2) break;
    *bar = 0;
    bar = strrchr(buf, '|');
  }
  if (titleWidth(g, buf) > max_w) {
    char first[48];
    const char* rest = strchr(buf, '|');
    if (rest == nullptr) rest = "";
    snprintf(first, sizeof(first), "%.*s", static_cast<int>(strlen(buf) - strlen(rest)), buf);
    char cand[56];
    for (int n = static_cast<int>(strlen(first)) - 1; n >= 1; --n) {
      while (n > 1 && first[n - 1] == ' ') --n;  // no space before the "..."
      snprintf(cand, sizeof(cand), "%.*s...%s", n, first, rest);
      if (titleWidth(g, cand) <= max_w || n == 1) {
        snprintf(buf, sizeof(buf), "%s", cand);
        break;
      }
    }
  }
  char spaced[64];
  spacedTitle(buf, spaced, sizeof(spaced));
  const int w = g.textWidth(spaced);
  const int x = cx - w / 2;
  const int h = displayFontHeight(g, titleStyle());
  g.setTextColor(color);
  g.setTextDatum(TextDatum::TopLeft);
  g.drawString(spaced, x, y);
  // Measured between letters: a string of only spaces measures (almost) zero.
  const int gap_w = g.textWidth("a   b") - g.textWidth("ab");
  char prefix[64];
  for (const char* p = strchr(spaced, ' '); p != nullptr; p = strchr(p + 1, ' ')) {
    if (strncmp(p, kSepGap, 3) != 0) continue;
    const size_t n = static_cast<size_t>(p - spaced);
    snprintf(prefix, sizeof(prefix), "%.*s", static_cast<int>(n), spaced);
    g.fillCircle(x + g.textWidth(prefix) + gap_w / 2, y + (h * 11) / 20, kDotR, color);
    p += 2;
  }
}

Rect layoutBanner(PlaneGfx& g, const Banner& b) {
  const int th = displayFontHeight(g, titleStyle());
  const int dh = displayFontHeight(g, detailStyle());
  displayFontApply(g, detailStyle());
  int w = g.textWidth(b.detail);
  const int tw = titleWidth(g, b.title);
  if (tw > w) w = tw;
  w += 2 * kPadX;
  const int h = th + dh + kLineGap + 2 * kPadY;
  if (w < h * 3) w = h * 3;
  const int max_w = maxBannerWidth(h);
  if (w > max_w) w = max_w;
  Rect r;
  r.w = w;
  r.h = h;
  r.x = radar::kCenterX - w / 2;
  r.y = kBannerTopY;
  return r;
}

void fillPill(PlaneGfx& g, const Rect& r, int grow, uint16_t c) {
  const int rad = r.h / 2 + grow;
  const int cy = r.y + r.h / 2;
  g.fillCircle(r.x + r.h / 2, cy, rad, c);
  g.fillCircle(r.x + r.w - r.h / 2 - 1, cy, rad, c);
  g.fillRect(r.x + r.h / 2, r.y - grow, r.w - r.h, r.h + 2 * grow, c);
}

void drawBanner(PlaneGfx& g, const Banner& b, const Rect& r) {
  const uint16_t accent = accentFor(g, b.kind);
  const uint16_t dark = g.color565(14, 18, 26);
  const uint16_t white = g.color565(255, 255, 255);
  fillPill(g, r, kOutline, b.urgent ? dark : accent);  // outline
  fillPill(g, r, 0, b.urgent ? accent : dark);
  const uint16_t title_c = b.urgent ? dark : accent;
  const uint16_t detail_c = b.urgent ? dark : white;
  const int th = displayFontHeight(g, titleStyle());
  const int max_w = r.w - 2 * kPadX;
  drawTitle(g, b.title, radar::kCenterX, r.y + kPadY, max_w, title_c);
  displayFontApply(g, detailStyle());
  g.setTextColor(detail_c);
  g.setTextDatum(TextDatum::TopCenter);
  g.drawString(b.detail, radar::kCenterX, r.y + kPadY + th + kLineGap);
  g.setTextDatum(TextDatum::TopLeft);
}

/** ISS symbol: a body with two solar panels, tilted along the direction of travel. */
void drawIssMarker(PlaneGfx& g, int x, int y) {
  const uint16_t accent = g.color565(90, 200, 255);
  const uint16_t dark = g.color565(14, 18, 26);
  const int body = ui::sc(5);
  const int pw = ui::sc(10);
  const int ph = ui::sc(6);
  const int o = ui::sc(1) + 1;
  // Outline first so it reads on light maps.
  g.fillRect(x - body - pw - o, y - ph - o, 2 * (body + pw + o), 2 * (ph + o), dark);
  g.fillRect(x - body - pw, y - ph, pw, 2 * ph, accent);   // left panel
  g.fillRect(x + body, y - ph, pw, 2 * ph, accent);        // right panel
  g.fillRect(x - body, y - ui::sc(1), 2 * body, ui::sc(2), dark);
  g.fillCircle(x, y, body - 1, g.color565(255, 255, 255));
}

Rect markerRect(int x, int y) {
  const int hw = ui::sc(5) + ui::sc(10) + ui::sc(3);
  const int hh = ui::sc(6) + ui::sc(3);
  return Rect{x - hw, y - hh, 2 * hw, 2 * hh};
}

Rect unite(const Rect& a, const Rect& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  const int x0 = a.x < b.x ? a.x : b.x;
  const int y0 = a.y < b.y ? a.y : b.y;
  const int x1 = (a.x + a.w) > (b.x + b.w) ? a.x + a.w : b.x + b.w;
  const int y1 = (a.y + a.h) > (b.y + b.h) ? a.y + a.h : b.y + b.h;
  return Rect{x0, y0, x1 - x0, y1 - y0};
}

Rect grown(const Rect& r, int by) {
  if (r.empty()) return r;
  return Rect{r.x - by, r.y - by, r.w + 2 * by, r.h + 2 * by};
}

DrawState computeState() {
  DrawState st;
  st.banner = services::sky::banner();
  if (st.banner.kind != EventKind::None) {
    st.rect = layoutBanner(tft, st.banner);
  }
  float az = 0, el = 0;
  if (services::sky::issEnabled() && services::sky::issLookNow(&az, &el)) {
    // Screen angle: azimuth relative to the radar's "up" direction.
    const float a = (az - static_cast<float>(radar::facingDeg())) * 3.14159265f / 180.0f;
    const float r = static_cast<float>(radar::kGridOuterRadius + ui::sc(9));
    st.marker = true;
    st.mx = radar::kCenterX + static_cast<int>(lroundf(sinf(a) * r));
    st.my = radar::kCenterY - static_cast<int>(lroundf(cosf(a) * r));
    st.m_el = el;
  }
  return st;
}

bool sameState(const DrawState& a, const DrawState& b) {
  return a.banner.kind == b.banner.kind && a.banner.urgent == b.banner.urgent &&
         strcmp(a.banner.title, b.banner.title) == 0 &&
         strcmp(a.banner.detail, b.banner.detail) == 0 && a.marker == b.marker &&
         a.mx == b.mx && a.my == b.my;
}

Rect stateRect(const DrawState& s) {
  Rect r = grown(s.rect, ui::sc(4));
  if (s.marker) r = unite(r, markerRect(s.mx, s.my));
  return r;
}

// --- Detail page -------------------------------------------------------------------

constexpr int kBezelInsetPx = ui::sc(10);

int halfWidthAt(int y, int h) {
  const int r = config::kDisplayWidth / 2 - kBezelInsetPx;
  const int dy = y + h / 2 - config::kDisplayHeight / 2;
  if (std::abs(dy) >= r) return 0;
  return static_cast<int>(std::sqrt(static_cast<float>(r * r - dy * dy))) - ui::sc(8);
}

void line(const char* text, int* y, UiTextStyle st, uint16_t c, int gap = ui::sc(4)) {
  displayFontApply(tft, st);
  const int h = displayFontHeight(tft, st);
  const int max_w = 2 * halfWidthAt(*y, h);
  char buf[72];
  strncpy(buf, text, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  while (tft.textWidth(buf) > max_w && strlen(buf) > 4) {
    const size_t n = strlen(buf);
    buf[n - 4] = '.';
    buf[n - 3] = '.';
    buf[n - 2] = '.';
    buf[n - 1] = '\0';
  }
  tft.setTextColor(c);
  tft.setTextDatum(TextDatum::TopCenter);
  tft.drawString(buf, config::kDisplayWidth / 2, *y);
  *y += h + gap;
}

int64_t nowUnix() {
  const time_t t = time(nullptr);
  return t > 1700000000 ? static_cast<int64_t>(t) : 0;
}

/** "Today 6:41 PM", "Tomorrow 7:02 AM", "Thu 6:53 PM". */
void formatDay(int64_t when, char* out, size_t len) {
  char clock[16];
  services::clock::formatClockFromEpoch(when, clock, sizeof(clock));
  const int32_t off = services::clock::timezoneOffsetSec();
  const int64_t day_when = (when + off) / 86400;
  const int64_t day_now = (nowUnix() + off) / 86400;
  if (day_when == day_now) {
    snprintf(out, len, "Today %s", clock);
  } else if (day_when == day_now + 1) {
    snprintf(out, len, "Tomorrow %s", clock);
  } else {
    static const char* const kDays[] = {"Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed"};
    snprintf(out, len, "%s %s", kDays[((day_when % 7) + 7) % 7], clock);
  }
}

uint16_t statusColor(uint8_t id) {
  switch (id) {
    case 1: case 3: case 6: return tft.color565(90, 220, 120);   // go / success / in flight
    case 4: case 7: return tft.color565(255, 90, 80);            // failure
    default: return tft.color565(255, 190, 60);                  // TBD / hold / TBC
  }
}

void drawLaunchDetail(const services::sky::Launch& l, int64_t now) {
  const uint16_t accent = accentFor(tft, EventKind::Launch);
  const uint16_t white = tft.color565(255, 255, 255);
  const uint16_t label = tft.color565(180, 200, 220);
  const uint16_t hint = tft.color565(120, 140, 160);
  char buf[72];
  char a[24];
  char b[24];
  int y = ui::sc(56);
  line("Rocket launch", &y, displayFontBody(), accent, ui::sc(6));
  line(l.rocket, &y, displayFontTitle(), white);
  line(l.mission[0] ? l.mission : "Mission TBA", &y, displayFontBody(), white, ui::sc(8));
  snprintf(buf, sizeof(buf), "%s, %s", l.pad, l.site);
  line(buf, &y, displayFontDetail(), label, ui::sc(8));

  formatDay(l.net, buf, sizeof(buf));
  if (l.window_end > l.window_start + 60) {
    services::clock::formatClockFromEpoch(l.window_start, a, sizeof(a));
    services::clock::formatClockFromEpoch(l.window_end, b, sizeof(b));
    char day[32];
    formatDay(l.window_start, day, sizeof(day));
    char* sp = strchr(day, ' ');
    if (sp) *sp = '\0';
    snprintf(buf, sizeof(buf), "Window %s %s - %s", day, a, b);
  } else {
    char t[40];
    formatDay(l.net, t, sizeof(t));
    snprintf(buf, sizeof(buf), "T-0 %s", t);
  }
  line(buf, &y, displayFontDetail(), white);
  line(l.status[0] ? l.status : "Status unknown", &y, displayFontBody(), statusColor(l.status_id),
       ui::sc(8));

  const int64_t dt = l.net - now;
  if (dt > 0 && l.status_id != 2) {
    const int h = static_cast<int>(dt / 3600);
    const int m = static_cast<int>((dt / 60) % 60);
    const int s = static_cast<int>(dt % 60);
    if (h >= 48) snprintf(buf, sizeof(buf), "T-%d days", h / 24);
    else if (h > 0) snprintf(buf, sizeof(buf), "T-%d:%02d:%02d", h, m, s);
    else snprintf(buf, sizeof(buf), "T-%d:%02d", m, s);
    line(buf, &y, displayFontTitle(), accent, ui::sc(6));
  } else if (dt <= 0 && dt > -15 * 60) {
    line("Liftoff!", &y, displayFontTitle(), accent, ui::sc(6));
  }

  float bearing = 90.0f;
  float dist = 0.0f;
  services::sky::launchBearing(l, &bearing, &dist);
  snprintf(buf, sizeof(buf), "Look %s, low in the sky,", services::sky::compassWord(bearing));
  line(buf, &y, displayFontDetail(), white);
  snprintf(buf, sizeof(buf), "1-2 min after liftoff (%d mi)",
           static_cast<int>(lroundf(dist / 1.609344f)));
  line(buf, &y, displayFontDetail(), label);
  line("Data: thespacedevs.com", &y, displayFontScale(), hint);
}

void drawIssDetail(int64_t now) {
  const uint16_t accent = accentFor(tft, EventKind::Iss);
  const uint16_t white = tft.color565(255, 255, 255);
  const uint16_t label = tft.color565(180, 200, 220);
  const uint16_t hint = tft.color565(120, 140, 160);
  char buf[72];
  size_t n = 0;
  const services::sky::orbit::Pass* passes = services::sky::issPasses(&n);
  int y = ui::sc(72);
  line("Space Station pass", &y, displayFontBody(), accent, ui::sc(8));
  if (n == 0) {
    line("No visible pass in the next 3 days", &y, displayFontBody(), white);
    return;
  }
  const auto& p = passes[0];
  float az = 0, el = 0;
  if (services::sky::issLookNow(&az, &el)) {
    line("Overhead now", &y, displayFontTitle(), white);
    snprintf(buf, sizeof(buf), "Look %s, %d deg up", services::sky::compassWord(az),
             static_cast<int>(lroundf(el)));
    line(buf, &y, displayFontBody(), accent);
    char t[16];
    services::clock::formatClockFromEpoch(p.end, t, sizeof(t));
    snprintf(buf, sizeof(buf), "Ends in the %s at %s", services::sky::orbit::compass8(p.end_az), t);
    line(buf, &y, displayFontDetail(), label, ui::sc(10));
  } else {
    formatDay(p.start, buf, sizeof(buf));
    line(buf, &y, displayFontTitle(), white);
    const int64_t dt = p.start - now;
    if (dt < 3600) {
      snprintf(buf, sizeof(buf), "in %d:%02d", static_cast<int>(dt / 60), static_cast<int>(dt % 60));
      line(buf, &y, displayFontBody(), accent);
    }
    snprintf(buf, sizeof(buf), "Appears in the %s", services::sky::compassWord(p.start_az));
    line(buf, &y, displayFontDetail(), white);
    snprintf(buf, sizeof(buf), "Highest %d deg up, %s", static_cast<int>(lroundf(p.peak_el)),
             services::sky::orbit::compass8(p.peak_az));
    line(buf, &y, displayFontDetail(), white);
    snprintf(buf, sizeof(buf), "Visible %d min, ends %s",
             static_cast<int>((p.end - p.start + 59) / 60), services::sky::orbit::compass8(p.end_az));
    line(buf, &y, displayFontDetail(), white, ui::sc(10));
  }
  line("Looks like a bright, steady star", &y, displayFontDetail(), label);
  line("moving fast, no blinking", &y, displayFontDetail(), label, ui::sc(10));
  if (n > 1) {
    line("Next passes", &y, displayFontScale(), hint);
    for (size_t i = 1; i < n && i < 4; ++i) {
      char day[32];
      formatDay(passes[i].start, day, sizeof(day));
      snprintf(buf, sizeof(buf), "%s  %d deg", day, static_cast<int>(lroundf(passes[i].peak_el)));
      line(buf, &y, displayFontDetail(), label);
    }
  }
}

EventKind s_detail_kind = EventKind::None;

}  // namespace

void tick(bool radar_visible) {
  if (!radar_visible) {
    s_have_state = false;  // the next full radar draw paints whatever is current
    s_state = computeState();
    return;
  }
  const DrawState next = computeState();
  if (s_have_state && sameState(next, s_state)) return;
  const Rect dirty = unite(s_have_state ? stateRect(s_state) : Rect{}, stateRect(next));
  s_state = next;
  s_have_state = true;
  if (!dirty.empty()) {
    ui::radarDisplayInvalidateRect(dirty.x, dirty.y, dirty.w, dirty.h);
  }
}

void drawRadarOverlay(PlaneGfx& g) {
  if (s_state.marker) {
    drawIssMarker(g, s_state.mx, s_state.my);
  }
  if (s_state.banner.kind != EventKind::None && !s_state.rect.empty()) {
    drawBanner(g, s_state.banner, s_state.rect);
  }
}

bool bannerBounds(int* x, int* y, int* w, int* h) {
  if (s_state.banner.kind == EventKind::None || s_state.rect.empty()) return false;
  // The widest the banner can get, not its current width: the countdown changes
  // its width every second, and plane tags shouldn't move with it.
  const int bw = maxBannerWidth(s_state.rect.h);
  *x = radar::kCenterX - bw / 2 - kOutline;
  *y = s_state.rect.y - kOutline;
  *w = bw + 2 * kOutline;
  *h = s_state.rect.h + 2 * kOutline;
  return true;
}

bool bannerHit(int x, int y) {
  if (s_state.banner.kind == EventKind::None || s_state.rect.empty()) return false;
  const Rect r = grown(s_state.rect, ui::sc(10));
  if (x < r.x || x >= r.x + r.w || y < r.y || y >= r.y + r.h) return false;
  s_detail_kind = s_state.banner.kind;
  return true;
}

void detailDraw() {
  tft.beginOffscreen();
  tft.fillScreen(tft.color565(0, 0, 0));
  const int64_t now = nowUnix();
  const services::sky::Launch* l = services::sky::currentLaunch();
  if (s_detail_kind == EventKind::Launch && l != nullptr) {
    drawLaunchDetail(*l, now);
  } else {
    drawIssDetail(now);
  }
  int y = config::kDisplayHeight - ui::sc(46);
  line("Tap to close", &y, displayFontScale(), tft.color565(120, 140, 160));
  tft.setTextDatum(TextDatum::TopLeft);
  tft.endOffscreen();
  s_detail_last_ms = millis();
  s_detail_last_sec = now;
}

void detailTick() {
  if (nowUnix() != s_detail_last_sec && millis() - s_detail_last_ms >= 900) {
    detailDraw();
  }
}

}  // namespace ui::sky

#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C
