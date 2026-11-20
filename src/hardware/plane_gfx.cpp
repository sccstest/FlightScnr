// Hot pixel code (sprite fills, AA text, panel blits): the framework builds with -Os,
// which keeps per-pixel loops as calls and real divides. Must come before the includes
// so inline helpers from headers get the same level.
#pragma GCC optimize("O2")

#include "hardware/plane_gfx.h"

#include <Arduino.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <esp_heap_caps.h>

#include "Arduino_TFT.h"

#include "hardware/gfx_log.h"

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static SemaphoreHandle_t s_panel_mutex = nullptr;

namespace {

class GfxWriteLineAccess : public Arduino_GFX {
 public:
  static void writeLineOn(Arduino_GFX* gfx, int16_t x0, int16_t y0, int16_t x1,
                          int16_t y1, uint16_t color) {
    static_cast<GfxWriteLineAccess*>(gfx)->writeLine(x0, y0, x1, y1, color);
  }
};

class SpriteCanvas : public Arduino_GFX {
 public:
  SpriteCanvas(uint16_t* buffer, int16_t w, int16_t h)
      : Arduino_GFX(w, h), buffer_(buffer), width_(w), height_(h) {}

  bool begin(int32_t speed = GFX_NOT_DEFINED) override {
    (void)speed;
    return true;
  }

  void writePixelPreclipped(int16_t x, int16_t y, uint16_t color) override {
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
      return;
    }
    buffer_[static_cast<size_t>(y) * static_cast<size_t>(width_) +
            static_cast<size_t>(x)] = color;
  }

  // Arduino_GFX's defaults for these go pixel by pixel through two virtual calls and
  // a bounds check each. Same clipping (to the canvas), but whole rows at once.
  void writeFillRectPreclipped(int16_t x, int16_t y, int16_t w, int16_t h,
                               uint16_t color) override {
    int32_t x0 = x;
    int32_t y0 = y;
    int32_t x1 = static_cast<int32_t>(x) + w;
    int32_t y1 = static_cast<int32_t>(y) + h;
    if (!clipRect(&x0, &y0, &x1, &y1)) {
      return;
    }
    const size_t n = static_cast<size_t>(x1 - x0);
    uint16_t* row = buffer_ + static_cast<size_t>(y0) * static_cast<size_t>(width_) +
                    static_cast<size_t>(x0);
    for (int32_t r = y0; r < y1; ++r, row += width_) {
      std::fill_n(row, n, color);
    }
  }

  void writeFastHLine(int16_t x, int16_t y, int16_t w, uint16_t color) override {
    if (w <= 0) {
      return;
    }
    writeFillRectPreclipped(x, y, w, 1, color);
  }

  void writeFastVLine(int16_t x, int16_t y, int16_t h, uint16_t color) override {
    if (h <= 0 || x < 0 || x >= width_) {
      return;
    }
    int32_t y0 = std::max<int32_t>(0, y);
    const int32_t y1 = std::min<int32_t>(height_, static_cast<int32_t>(y) + h);
    uint16_t* p = buffer_ + static_cast<size_t>(y0) * static_cast<size_t>(width_) +
                  static_cast<size_t>(x);
    for (; y0 < y1; ++y0, p += width_) {
      *p = color;
    }
  }

  using Arduino_GFX::draw16bitRGBBitmap;

  void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap, int16_t w,
                          int16_t h) override {
    copyBitmap(x, y, bitmap, w, h, false, 0);
  }

  void draw16bitRGBBitmap(int16_t x, int16_t y, uint16_t* bitmap,
                          uint16_t transparent_color, int16_t w, int16_t h) override {
    copyBitmap(x, y, bitmap, w, h, true, transparent_color);
  }

 private:
  bool clipRect(int32_t* x0, int32_t* y0, int32_t* x1, int32_t* y1) const {
    if (*x0 < 0) *x0 = 0;
    if (*y0 < 0) *y0 = 0;
    if (*x1 > width_) *x1 = width_;
    if (*y1 > height_) *y1 = height_;
    return *x0 < *x1 && *y0 < *y1;
  }

  void copyBitmap(int16_t x, int16_t y, const uint16_t* bitmap, int16_t w, int16_t h,
                  bool keyed, uint16_t key) {
    if (bitmap == nullptr || w <= 0 || h <= 0) {
      return;
    }
    int32_t x0 = x;
    int32_t y0 = y;
    int32_t x1 = static_cast<int32_t>(x) + w;
    int32_t y1 = static_cast<int32_t>(y) + h;
    if (!clipRect(&x0, &y0, &x1, &y1)) {
      return;
    }
    const size_t n = static_cast<size_t>(x1 - x0);
    for (int32_t r = y0; r < y1; ++r) {
      const uint16_t* src = bitmap + static_cast<size_t>(r - y) * static_cast<size_t>(w) +
                            static_cast<size_t>(x0 - x);
      uint16_t* dst = buffer_ + static_cast<size_t>(r) * static_cast<size_t>(width_) +
                      static_cast<size_t>(x0);
      if (!keyed) {
        memmove(dst, src, n * sizeof(uint16_t));
        continue;
      }
      for (size_t i = 0; i < n; ++i) {
        if (src[i] != key) {
          dst[i] = src[i];
        }
      }
    }
  }

  uint16_t* buffer_;
  int16_t width_;
  int16_t height_;
};

uint16_t* s_blit_scratch = nullptr;
size_t s_blit_scratch_pixels = 0;

bool ensureBlitScratch(size_t pixels) {
  if (pixels == 0) {
    return false;
  }
  if (s_blit_scratch != nullptr && s_blit_scratch_pixels >= pixels) {
    return true;
  }
  if (s_blit_scratch != nullptr) {
    heap_caps_free(s_blit_scratch);
    s_blit_scratch = nullptr;
    s_blit_scratch_pixels = 0;
  }
  const size_t bytes = pixels * sizeof(uint16_t);
  s_blit_scratch = static_cast<uint16_t*>(
      heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (s_blit_scratch == nullptr) {
    s_blit_scratch = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  }
  if (s_blit_scratch == nullptr) {
    return false;
  }
  s_blit_scratch_pixels = pixels;
  return true;
}

/** Expand blit bounds outward to 2×2 grid (CO5300). alignDrawArea2 shrinks inward and
 *  can skip thin erase regions or miss 2×2 sweep stamps. */
bool expandBlitAreaForPixelAlign2(int16_t* x, int16_t* y, int16_t* w, int16_t* h,
                                  int16_t screen_w, int16_t screen_h) {
  if (!Arduino_TFT::pixelAlign2()) {
    return *w > 0 && *h > 0;
  }

  int16_t x0 = static_cast<int16_t>((*x) & ~1);
  int16_t y0 = static_cast<int16_t>((*y) & ~1);
  int16_t x2 = static_cast<int16_t>(*x + *w);
  int16_t y2 = static_cast<int16_t>(*y + *h);
  if (x2 & 1) {
    x2++;
  }
  if (y2 & 1) {
    y2++;
  }

  *x = x0;
  *y = y0;
  *w = static_cast<int16_t>(x2 - x0);
  *h = static_cast<int16_t>(y2 - y0);

  if (*x < 0) {
    *w = static_cast<int16_t>(*w + *x);
    *x = 0;
  }
  if (*y < 0) {
    *h = static_cast<int16_t>(*h + *y);
    *y = 0;
  }
  if (*x + *w > screen_w) {
    *w = static_cast<int16_t>(screen_w - *x);
  }
  if (*y + *h > screen_h) {
    *h = static_cast<int16_t>(screen_h - *y);
  }

  x0 = static_cast<int16_t>((*x) & ~1);
  y0 = static_cast<int16_t>((*y) & ~1);
  x2 = static_cast<int16_t>(*x + *w);
  y2 = static_cast<int16_t>(*y + *h);
  if (x2 & 1) {
    x2 = static_cast<int16_t>(x2 < screen_w ? x2 + 1 : x2 - 1);
  }
  if (y2 & 1) {
    y2 = static_cast<int16_t>(y2 < screen_h ? y2 + 1 : y2 - 1);
  }

  *x = x0;
  *y = y0;
  *w = static_cast<int16_t>(x2 - x0);
  *h = static_cast<int16_t>(y2 - y0);
  return *w >= 2 && *h >= 2;
}

void blitRegionWithTransparentKey(int16_t x, int16_t y, int16_t w, int16_t h,
                                const uint16_t* src, int16_t src_stride,
                                uint16_t transparent_key, PlaneGfx* gfx) {
  if (gfx == nullptr || gfx->raw() == nullptr || src == nullptr || w <= 0 || h <= 0 ||
      src_stride <= 0) {
    return;
  }

  auto* panel = static_cast<Arduino_TFT*>(gfx->raw());
  const int16_t screen_w = static_cast<int16_t>(gfx->raw()->width());
  const int16_t screen_h = static_cast<int16_t>(gfx->raw()->height());

  for (int16_t row = 0; row < h; ++row) {
    const int16_t dst_y = static_cast<int16_t>(y + row);
    if (dst_y < 0 || dst_y >= screen_h) {
      continue;
    }
    const uint16_t* row_src =
        src + static_cast<size_t>(row) * static_cast<size_t>(src_stride);
    int16_t col = 0;
    while (col < w) {
      while (col < w && row_src[col] == transparent_key) {
        ++col;
      }
      const int16_t run_start = col;
      while (col < w && row_src[col] != transparent_key) {
        ++col;
      }
      const int16_t run_len = static_cast<int16_t>(col - run_start);
      if (run_len <= 0) {
        continue;
      }
      int16_t dst_x = static_cast<int16_t>(x + run_start);
      int16_t len = run_len;
      if (dst_x < 0) {
        len = static_cast<int16_t>(len + dst_x);
        dst_x = 0;
      }
      if (dst_x >= screen_w || len <= 0) {
        continue;
      }
      if (dst_x + len > screen_w) {
        len = static_cast<int16_t>(screen_w - dst_x);
      }
      panel->writeAddrWindow(dst_x, dst_y, static_cast<uint16_t>(len), 1);
      panel->writePixels(const_cast<uint16_t*>(row_src + run_start),
                         static_cast<uint32_t>(len));
    }
  }
}

}  // namespace

void planeGfxPanelLockInit() {
  if (s_panel_mutex == nullptr) {
    s_panel_mutex = xSemaphoreCreateMutex();
  }
}

bool PlaneGfx::targetUsesPixelAlign2() const {
  return Arduino_TFT::pixelAlign2() && hardware_panel_;
}

void PlaneGfx::drawLinePixelAlign2(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                   uint16_t color) {
  int16_t dx = static_cast<int16_t>(abs(x1 - x0));
  int16_t sx = x0 < x1 ? 1 : -1;
  int16_t dy = static_cast<int16_t>(-abs(y1 - y0));
  int16_t sy = y0 < y1 ? 1 : -1;
  int16_t err = dx + dy;

  for (;;) {
    const int16_t ax = static_cast<int16_t>(x0 & ~1);
    const int16_t ay = static_cast<int16_t>(y0 & ~1);
    gfx_->fillRect(ax, ay, 2, 2, color);
    if (x0 == x1 && y0 == y1) {
      break;
    }
    const int16_t e2 = static_cast<int16_t>(2 * err);
    if (e2 >= dy) {
      err = static_cast<int16_t>(err + dy);
      x0 = static_cast<int16_t>(x0 + sx);
    }
    if (e2 <= dx) {
      err = static_cast<int16_t>(err + dx);
      y0 = static_cast<int16_t>(y0 + sy);
    }
  }
}

void PlaneGfx::fillScreen(uint16_t color) {
  if (gfx_ == nullptr) {
    return;
  }
  // Hardware panel: one GFX fillRect session; avoid nested startWrite + raw writeRepeat.
  if (hardware_panel_) {
    gfx_->fillScreen(color);
    return;
  }
  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  gfx_->fillScreen(color);
  if (opened_here) {
    endWrite();
  }
}

void PlaneGfx::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint16_t color) {
  if (gfx_ == nullptr) {
    return;
  }
  // Arduino_GFX::fillRect already wraps startWrite/endWrite; nesting breaks QSPI CS.
  if (hardware_panel_) {
    gfx_->fillRect(x, y, w, h, color);
    return;
  }
  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  gfx_->fillRect(x, y, w, h, color);
  if (opened_here) {
    endWrite();
  }
}

void PlaneGfx::fillCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {
  if (gfx_ != nullptr) {
    gfx_->fillCircle(x, y, r, color);
  }
}

void PlaneGfx::drawCircle(int16_t x, int16_t y, int16_t r, uint16_t color) {
  if (gfx_ != nullptr) {
    gfx_->drawCircle(x, y, r, color);
  }
}

void PlaneGfx::fillArc(int16_t x, int16_t y, int16_t r_outer, int16_t r_inner,
                       float start_deg, float end_deg, uint16_t color) {
  if (gfx_ != nullptr) {
    gfx_->fillArc(x, y, r_outer, r_inner, start_deg, end_deg, color);
  }
}

void PlaneGfx::fillTriangle(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            int16_t x2, int16_t y2, uint16_t color) {
  if (gfx_ != nullptr) {
    gfx_->fillTriangle(x0, y0, x1, y1, x2, y2, color);
  }
}

void PlaneGfx::drawLineInternal(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                                uint16_t color) {
  if (targetUsesPixelAlign2()) {
    drawLinePixelAlign2(x0, y0, x1, y1, color);
    return;
  }
  GfxWriteLineAccess::writeLineOn(gfx_, x0, y0, x1, y1, color);
}

void PlaneGfx::drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                        uint16_t color) {
  if (gfx_ == nullptr) {
    return;
  }
  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  if (targetUsesPixelAlign2()) {
    drawLinePixelAlign2(x0, y0, x1, y1, color);
  } else {
    drawLineInternal(x0, y0, x1, y1, color);
  }
  if (opened_here) {
    endWrite();
  }
}

void PlaneGfx::drawWideLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1,
                            float half_width, uint16_t color) {
  if (gfx_ == nullptr) {
    return;
  }
  const int steps = std::max(1, static_cast<int>(half_width * 2.0f + 0.5f));
  const float offset = -half_width;

  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  for (int i = 0; i < steps; ++i) {
    const float t = offset + static_cast<float>(i);
    const int ox = static_cast<int>(std::lround(t));
    const int oy = static_cast<int>(std::lround(-t));
    drawLineInternal(x0 + ox, y0 + oy, x1 + ox, y1 + oy, color);
  }
  if (opened_here) {
    endWrite();
  }
}

uint16_t PlaneGfx::color565(uint8_t r, uint8_t g, uint8_t b) const {
  if (gfx_ != nullptr) {
    return gfx_->color565(r, g, b);
  }
  return static_cast<uint16_t>(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

void PlaneGfx::setTextColor(uint16_t fg) {
  text_fg_ = fg;
  text_bg_ = fg;
  text_transparent_ = true;
  if (gfx_ != nullptr) {
    gfx_->setTextColor(fg);
  }
}

void PlaneGfx::setTextColor(uint16_t fg, uint16_t bg) {
  text_fg_ = fg;
  text_bg_ = bg;
  text_transparent_ = (fg == bg);
  if (gfx_ != nullptr) {
    gfx_->setTextColor(fg, bg);
  }
}

void PlaneGfx::setTextSize(uint8_t size) {
  if (gfx_ != nullptr) {
    gfx_->setTextSize(size);
  }
}

void PlaneGfx::setFont(const GFXfont* font) {
  aa_font_ = nullptr;
  if (gfx_ != nullptr) {
    gfx_->setFont(font);
  }
}

void PlaneGfx::setFont(const AaFont* font) {
  aa_font_ = font;
  if (gfx_ != nullptr) {
    gfx_->setFont(static_cast<const GFXfont*>(nullptr));
  }
}

void PlaneGfx::setTextDatum(TextDatum datum) { datum_ = datum; }

void PlaneGfx::setTextWrap(bool wrap) {
  if (gfx_ != nullptr) {
    gfx_->setTextWrap(wrap);
  }
}

namespace {

/** v / 15 for v <= 945 (63 * 15), exact: 4370 / 65536 overshoots 1/15 by less than
 *  1/15 / 4681, so the floor never moves. Avoids three real divides per pixel. */
inline uint32_t div15(uint32_t v) { return (v * 4370u) >> 16; }

/** (fg * a + bg * (15 - a)) / 15 per channel, 4-bit alpha. Same results as the old
 *  divide-by-15 version, so text looks exactly as before. */
inline uint16_t blend565(uint16_t fg, uint16_t bg, uint8_t a4) {
  if (a4 == 0) {
    return bg;
  }
  if (a4 >= 15) {
    return fg;
  }
  const uint32_t ia = 15u - a4;
  const uint32_t r = div15(((fg >> 11) & 0x1Fu) * a4 + ((bg >> 11) & 0x1Fu) * ia);
  const uint32_t g = div15(((fg >> 5) & 0x3Fu) * a4 + ((bg >> 5) & 0x3Fu) * ia);
  const uint32_t b = div15((fg & 0x1Fu) * a4 + (bg & 0x1Fu) * ia);
  return static_cast<uint16_t>((r << 11) | (g << 5) | b);
}

// ---- [perf] counters ------------------------------------------------------------
struct PerfSlot {
  uint32_t n = 0;
  uint32_t us = 0;
  uint32_t max_us = 0;
};
PerfSlot s_perf[static_cast<size_t>(gfx_perf::Kind::kCount)];
unsigned long s_perf_last_ms = 0;
constexpr unsigned long kPerfReportMs = 10000;

}  // namespace

namespace gfx_perf {

void add(Kind kind, uint32_t us) {
  PerfSlot& s = s_perf[static_cast<size_t>(kind)];
  ++s.n;
  s.us += us;
  if (us > s.max_us) {
    s.max_us = us;
  }
  const unsigned long now = millis();
  if (s_perf_last_ms == 0) {
    s_perf_last_ms = now;
    return;
  }
  if (now - s_perf_last_ms < kPerfReportMs) {
    return;
  }
  s_perf_last_ms = now;
  const PerfSlot& ic = s_perf[static_cast<size_t>(Kind::Icon)];
  const PerfSlot& im = s_perf[static_cast<size_t>(Kind::IconMiss)];
  const PerfSlot& tx = s_perf[static_cast<size_t>(Kind::Text)];
  const PerfSlot& fl = s_perf[static_cast<size_t>(Kind::Flush)];
  // icon: every icon drawn (miss = rotated mask built, n of them included in icon);
  // text: AA strings; flush: panel blits of 1/3 screen or more. ms = total, max = worst one.
  Serial.printf(
      "[perf] icon n=%lu ms=%lu.%lu (miss n=%lu ms=%lu.%lu) text n=%lu ms=%lu.%lu max=%luus "
      "flush n=%lu ms=%lu.%lu max=%luus\n",
      static_cast<unsigned long>(ic.n), static_cast<unsigned long>(ic.us / 1000),
      static_cast<unsigned long>((ic.us % 1000) / 100), static_cast<unsigned long>(im.n),
      static_cast<unsigned long>(im.us / 1000), static_cast<unsigned long>((im.us % 1000) / 100),
      static_cast<unsigned long>(tx.n), static_cast<unsigned long>(tx.us / 1000),
      static_cast<unsigned long>((tx.us % 1000) / 100), static_cast<unsigned long>(tx.max_us),
      static_cast<unsigned long>(fl.n), static_cast<unsigned long>(fl.us / 1000),
      static_cast<unsigned long>((fl.us % 1000) / 100), static_cast<unsigned long>(fl.max_us));
  for (PerfSlot& slot : s_perf) {
    slot = PerfSlot{};
  }
}

}  // namespace gfx_perf

void PlaneGfx::getAaTextBounds(const char* text, int16_t x, int16_t y, int16_t* x1,
                               int16_t* y1, uint16_t* w, uint16_t* h) const {
  *x1 = x;
  *y1 = y;
  *w = 0;
  *h = 0;
  if (aa_font_ == nullptr || text == nullptr || *text == '\0') {
    return;
  }

  int16_t min_x = 0x7FFF;
  int16_t min_y = 0x7FFF;
  int16_t max_x = -0x7FFF;
  int16_t max_y = -0x7FFF;
  int16_t cursor_x = x;
  const int16_t cursor_y = y;
  bool any = false;

  for (const char* p = text; *p != '\0'; ++p) {
    const uint8_t c = static_cast<uint8_t>(*p);
    if (c < aa_font_->first || c > aa_font_->last) {
      cursor_x = static_cast<int16_t>(cursor_x + aa_font_->yAdvance / 2);
      continue;
    }
    const AaGlyph& g = aa_font_->glyph[c - aa_font_->first];
    if (g.width > 0 && g.height > 0) {
      const int16_t gx0 = static_cast<int16_t>(cursor_x + g.xOffset);
      const int16_t gy0 = static_cast<int16_t>(cursor_y + g.yOffset);
      const int16_t gx1 = static_cast<int16_t>(gx0 + g.width - 1);
      const int16_t gy1 = static_cast<int16_t>(gy0 + g.height - 1);
      if (gx0 < min_x) {
        min_x = gx0;
      }
      if (gy0 < min_y) {
        min_y = gy0;
      }
      if (gx1 > max_x) {
        max_x = gx1;
      }
      if (gy1 > max_y) {
        max_y = gy1;
      }
      any = true;
    }
    cursor_x = static_cast<int16_t>(cursor_x + g.xAdvance);
  }

  if (!any) {
    return;
  }
  *x1 = min_x;
  *y1 = min_y;
  *w = static_cast<uint16_t>(max_x - min_x + 1);
  *h = static_cast<uint16_t>(max_y - min_y + 1);
}

int PlaneGfx::textWidth(const char* text) const {
  if (text == nullptr) {
    return 0;
  }
  if (aa_font_ != nullptr) {
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    getAaTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    return static_cast<int>(w);
  }
  if (gfx_ == nullptr) {
    return 0;
  }
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  gfx_->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  return static_cast<int>(w);
}

bool PlaneGfx::averageLuma(int16_t x, int16_t y, int16_t w, int16_t h,
                           uint8_t* out) const {
  if (out == nullptr || fb_ == nullptr || w <= 0 || h <= 0 || fb_w_ <= 0 || fb_h_ <= 0) {
    return false;
  }
  int x0 = x;
  int y0 = y;
  int x1 = static_cast<int>(x) + w;
  int y1 = static_cast<int>(y) + h;
  if (x0 < 0) {
    x0 = 0;
  }
  if (y0 < 0) {
    y0 = 0;
  }
  if (x1 > fb_w_) {
    x1 = fb_w_;
  }
  if (y1 > fb_h_) {
    y1 = fb_h_;
  }
  if (x0 >= x1 || y0 >= y1) {
    return false;
  }

  const int step_x = std::max(1, (x1 - x0) / 6);
  const int step_y = std::max(1, (y1 - y0) / 4);
  uint32_t sum = 0;
  uint32_t n = 0;
  for (int py = y0; py < y1; py += step_y) {
    const uint16_t* row = fb_ + static_cast<size_t>(py) * static_cast<size_t>(fb_w_);
    for (int px = x0; px < x1; px += step_x) {
      const uint16_t c = row[px];
      const uint32_t r = ((c >> 11) & 0x1F) * 255u / 31u;
      const uint32_t g = ((c >> 5) & 0x3F) * 255u / 63u;
      const uint32_t b = (c & 0x1F) * 255u / 31u;
      sum += (r * 54u + g * 183u + b * 19u) >> 8;
      ++n;
    }
  }
  if (n == 0) {
    return false;
  }
  *out = static_cast<uint8_t>(sum / n);
  return true;
}

int PlaneGfx::fontHeight() const {
  if (aa_font_ != nullptr) {
    int16_t x1 = 0;
    int16_t y1 = 0;
    uint16_t w = 0;
    uint16_t h = 0;
    getAaTextBounds("Ag", 0, 0, &x1, &y1, &w, &h);
    return h > 0 ? static_cast<int>(h) : static_cast<int>(aa_font_->yAdvance);
  }
  if (gfx_ == nullptr) {
    return 8;
  }
  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  gfx_->getTextBounds("Ag", 0, 0, &x1, &y1, &w, &h);
  return static_cast<int>(h);
}

void PlaneGfx::mapDatum(const char* text, int16_t x, int16_t y, int16_t* out_x,
                        int16_t* out_y) const {
  if (text == nullptr) {
    *out_x = x;
    *out_y = y;
    return;
  }
  if (aa_font_ == nullptr && gfx_ == nullptr) {
    *out_x = x;
    *out_y = y;
    return;
  }

  int16_t x1 = 0;
  int16_t y1 = 0;
  uint16_t w = 0;
  uint16_t h = 0;
  if (aa_font_ != nullptr) {
    getAaTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  } else {
    gfx_->getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
  }

  switch (datum_) {
    case TextDatum::TopLeft:
      *out_x = x - x1;
      *out_y = y - y1;
      break;
    case TextDatum::TopCenter:
      *out_x = x - (x1 + static_cast<int16_t>(w) / 2);
      *out_y = y - y1;
      break;
    case TextDatum::TopRight:
      *out_x = x - (x1 + static_cast<int16_t>(w));
      *out_y = y - y1;
      break;
    case TextDatum::MiddleLeft:
      *out_x = x - x1;
      *out_y = y - (y1 + static_cast<int16_t>(h) / 2);
      break;
    case TextDatum::MiddleCenter:
      *out_x = x - (x1 + static_cast<int16_t>(w) / 2);
      *out_y = y - (y1 + static_cast<int16_t>(h) / 2);
      break;
    case TextDatum::MiddleRight:
      *out_x = x - (x1 + static_cast<int16_t>(w));
      *out_y = y - (y1 + static_cast<int16_t>(h) / 2);
      break;
    case TextDatum::BottomLeft:
      *out_x = x - x1;
      *out_y = y - (y1 + static_cast<int16_t>(h));
      break;
    case TextDatum::BottomCenter:
      *out_x = x - (x1 + static_cast<int16_t>(w) / 2);
      *out_y = y - (y1 + static_cast<int16_t>(h));
      break;
    case TextDatum::BottomRight:
      *out_x = x - (x1 + static_cast<int16_t>(w));
      *out_y = y - (y1 + static_cast<int16_t>(h));
      break;
  }
}

void PlaneGfx::drawAaString(const char* text, int16_t cursor_x, int16_t cursor_y) {
  if (gfx_ == nullptr || aa_font_ == nullptr || text == nullptr) {
    return;
  }

  int16_t x = cursor_x;
  const int16_t y = cursor_y;
  const uint16_t fg = text_fg_;
  const bool transparent = text_transparent_;
  const uint16_t opaque_bg = text_bg_;

  for (const char* p = text; *p != '\0'; ++p) {
    const uint8_t c = static_cast<uint8_t>(*p);
    if (c < aa_font_->first || c > aa_font_->last) {
      x = static_cast<int16_t>(x + aa_font_->yAdvance / 2);
      continue;
    }
    const AaGlyph& g = aa_font_->glyph[c - aa_font_->first];
    const int16_t gw = g.width;
    const int16_t gh = g.height;
    if (gw <= 0 || gh <= 0) {
      x = static_cast<int16_t>(x + g.xAdvance);
      continue;
    }

    const int16_t ox = static_cast<int16_t>(x + g.xOffset);
    const int16_t oy = static_cast<int16_t>(y + g.yOffset);

    if (fb_ != nullptr) {
      // RAM target (sprite / offscreen canvas): blend straight into its pixels. Same
      // result as the scratch + draw16bitRGBBitmap path below, without a per-pixel
      // trip through Arduino_GFX. Transparent text skips alpha-0 pixels; opaque
      // text still paints the whole glyph box (alpha 0 = background).
      const uint8_t* bm = aa_font_->bitmap + g.bitmapOffset;
      const int32_t c0 = std::max<int32_t>(0, -static_cast<int32_t>(ox));
      const int32_t c1 =
          std::min<int32_t>(gw, static_cast<int32_t>(fb_w_) - static_cast<int32_t>(ox));
      const int32_t r0 = std::max<int32_t>(0, -static_cast<int32_t>(oy));
      const int32_t r1 =
          std::min<int32_t>(gh, static_cast<int32_t>(fb_h_) - static_cast<int32_t>(oy));
      for (int32_t row = r0; row < r1; ++row) {
        uint16_t* dst = fb_ + static_cast<size_t>(oy + row) * static_cast<size_t>(fb_w_) +
                        static_cast<size_t>(ox);
        size_t bit_i = static_cast<size_t>(row) * static_cast<size_t>(gw) +
                       static_cast<size_t>(c0);
        for (int32_t col = c0; col < c1; ++col, ++bit_i) {
          const uint8_t byte = bm[bit_i >> 1];
          const uint8_t a4 = (bit_i & 1) ? static_cast<uint8_t>(byte & 0x0F)
                                         : static_cast<uint8_t>(byte >> 4);
          if (transparent) {
            if (a4 != 0) {
              dst[col] = blend565(fg, dst[col], a4);
            }
          } else {
            dst[col] = blend565(fg, opaque_bg, a4);
          }
        }
      }
      x = static_cast<int16_t>(x + g.xAdvance);
      continue;
    }

    const size_t pixels = static_cast<size_t>(gw) * static_cast<size_t>(gh);
    // Transparent text composites against the framebuffer in one blit. An
    // opaque background would stamp a solid chip behind every glyph.
    const bool can_blit = ensureBlitScratch(pixels) && (!transparent || fb_ != nullptr);

    if (can_blit) {
      const uint8_t* bm = aa_font_->bitmap + g.bitmapOffset;
      size_t bit_i = 0;
      for (int16_t row = 0; row < gh; ++row) {
        for (int16_t col = 0; col < gw; ++col) {
          const uint8_t byte = bm[bit_i / 2];
          const uint8_t a4 =
              (bit_i & 1) ? static_cast<uint8_t>(byte & 0x0F)
                          : static_cast<uint8_t>(byte >> 4);
          ++bit_i;
          uint16_t bg = opaque_bg;
          if (transparent) {
            const int16_t px = static_cast<int16_t>(ox + col);
            const int16_t py = static_cast<int16_t>(oy + row);
            if (px >= 0 && py >= 0 && px < fb_w_ && py < fb_h_) {
              bg = fb_[static_cast<size_t>(py) * static_cast<size_t>(fb_w_) +
                       static_cast<size_t>(px)];
            } else {
              bg = 0;
            }
          }
          s_blit_scratch[static_cast<size_t>(row) * static_cast<size_t>(gw) +
                         static_cast<size_t>(col)] = blend565(fg, bg, a4);
        }
      }
      draw16bitRGBBitmap(ox, oy, s_blit_scratch, gw, gh);
    } else {
      const uint8_t* bm = aa_font_->bitmap + g.bitmapOffset;
      size_t bit_i = 0;
      for (int16_t row = 0; row < gh; ++row) {
        for (int16_t col = 0; col < gw; ++col) {
          const uint8_t byte = bm[bit_i / 2];
          const uint8_t a4 =
              (bit_i & 1) ? static_cast<uint8_t>(byte & 0x0F)
                          : static_cast<uint8_t>(byte >> 4);
          ++bit_i;
          if (a4 == 0) {
            continue;
          }
          const int16_t px = static_cast<int16_t>(ox + col);
          const int16_t py = static_cast<int16_t>(oy + row);
          uint16_t bg = opaque_bg;
          if (transparent) {
            if (fb_ != nullptr && px >= 0 && py >= 0 && px < fb_w_ &&
                py < fb_h_) {
              bg = fb_[static_cast<size_t>(py) * static_cast<size_t>(fb_w_) +
                       static_cast<size_t>(px)];
            } else {
              bg = 0;
            }
          }
          gfx_->drawPixel(px, py, blend565(fg, bg, a4));
        }
      }
    }

    x = static_cast<int16_t>(x + g.xAdvance);
  }
}

void PlaneGfx::drawString(const char* text, int16_t x, int16_t y) {
  if (gfx_ == nullptr || text == nullptr) {
    return;
  }
  int16_t draw_x = x;
  int16_t draw_y = y;
  mapDatum(text, x, y, &draw_x, &draw_y);
  if (Arduino_TFT::pixelAlign2()) {
    draw_x &= ~1;
    draw_y &= ~1;
  }

  if (aa_font_ != nullptr) {
    const uint32_t t0 = micros();
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    // The P4 "panel" is a PSRAM framebuffer whose bus nests write sessions; one
    // session per string means one cache write-back instead of one per glyph pixel.
    const bool opened_here = write_depth_ == 0;
#else
    // QSPI panels: drawPixel opens its own CS session; nesting breaks CS.
    const bool opened_here = write_depth_ == 0 && !hardware_panel_;
#endif
    if (opened_here) {
      startWrite();
    }
    drawAaString(text, draw_x, draw_y);
    if (opened_here) {
      endWrite();
    }
    gfx_perf::add(gfx_perf::Kind::Text, micros() - t0);
    return;
  }

  gfx_->setCursor(draw_x, draw_y);
  // drawChar() opens/closes its own SPI session per glyph; do not wrap print().
  if (hardware_panel_) {
    gfx_->print(text);
    return;
  }
  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  gfx_->print(text);
  if (opened_here) {
    endWrite();
  }
}

void PlaneGfx::startWrite() {
  if (gfx_ == nullptr) {
    return;
  }
  if (write_depth_++ == 0) {
    if (hardware_panel_ && s_panel_mutex != nullptr) {
      xSemaphoreTake(s_panel_mutex, portMAX_DELAY);
    }
    gfx_->startWrite();
  }
}

void PlaneGfx::endWrite() {
  if (gfx_ == nullptr || write_depth_ == 0) {
    return;
  }
  if (--write_depth_ == 0) {
    gfx_->endWrite();
    if (hardware_panel_ && s_panel_mutex != nullptr) {
      xSemaphoreGive(s_panel_mutex);
    }
  }
}

bool PlaneGfx::beginOffscreen() {
  // Only worthwhile for the hardware panel when pixel writes are snapped to a
  // 2x2 grid (CO5300). On other panels direct drawing is already crisp, so skip
  // the buffer cost. Never reenter or hijack an already-open SPI session.
  if (offscreen_active_ || gfx_ == nullptr || !hardware_panel_ || write_depth_ != 0) {
    return false;
  }
  if (!Arduino_TFT::pixelAlign2()) {
    return false;
  }
  const int16_t w = static_cast<int16_t>(gfx_->width());
  const int16_t h = static_cast<int16_t>(gfx_->height());
  if (offscreen_canvas_ == nullptr) {
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(uint16_t);
    offscreen_buf_ =
        static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (offscreen_buf_ == nullptr) {
      offscreen_buf_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
    }
    if (offscreen_buf_ == nullptr) {
      return false;
    }
    offscreen_canvas_ = new SpriteCanvas(offscreen_buf_, w, h);
    offscreen_canvas_->setTextWrap(false);
  }
  // Pose as a non-hardware target so every draw op takes the canvas code paths
  // (identical to PlaneGfxSprite), which never touch Arduino_TFT pixelAlign2.
  saved_gfx_ = gfx_;
  saved_hardware_panel_ = hardware_panel_;
  gfx_ = offscreen_canvas_;
  hardware_panel_ = false;
  setFramebuffer(offscreen_buf_, w, h);
  offscreen_active_ = true;
  return true;
}

void PlaneGfx::endOffscreen() {
  if (!offscreen_active_) {
    return;
  }
  gfx_ = saved_gfx_;
  hardware_panel_ = saved_hardware_panel_;
  setFramebuffer(nullptr, 0, 0);
  offscreen_active_ = false;
  if (offscreen_buf_ == nullptr) {
    return;
  }
  const int16_t w = static_cast<int16_t>(gfx_->width());
  const int16_t h = static_cast<int16_t>(gfx_->height());
  blitRegionFromBuffer(0, 0, w, h, offscreen_buf_, w);
}

void PlaneGfx::endOffscreenRegion(int16_t x, int16_t y, int16_t w, int16_t h) {
  if (!offscreen_active_) {
    return;
  }
  gfx_ = saved_gfx_;
  hardware_panel_ = saved_hardware_panel_;
  setFramebuffer(nullptr, 0, 0);
  offscreen_active_ = false;
  if (offscreen_buf_ == nullptr || w <= 0 || h <= 0) {
    return;
  }

  const int16_t screen_w = static_cast<int16_t>(gfx_->width());
  const int16_t screen_h = static_cast<int16_t>(gfx_->height());
  const int16_t x0 = std::max<int16_t>(0, x);
  const int16_t y0 = std::max<int16_t>(0, y);
  const int16_t x1 = std::min<int16_t>(screen_w, static_cast<int16_t>(x + w));
  const int16_t y1 = std::min<int16_t>(screen_h, static_cast<int16_t>(y + h));
  if (x1 <= x0 || y1 <= y0) {
    return;
  }
  const uint16_t* src =
      offscreen_buf_ + static_cast<size_t>(y0) * screen_w + x0;
  blitRegionFromBuffer(x0, y0, x1 - x0, y1 - y0, src, screen_w);
}

void PlaneGfx::panelFlushBitmap(int16_t x, int16_t y, int16_t w, int16_t h,
                                const uint16_t* src) {
  if (gfx_ == nullptr || src == nullptr || w <= 0 || h <= 0) {
    return;
  }

  hardware::gfxLogf("[gfx] flush %dx%d @ (%d,%d) begin", w, h, x, y);

  if (!hardware_panel_) {
    gfx_->draw16bitRGBBitmap(x, y, const_cast<uint16_t*>(src), w, h);
    hardware::gfxLog("[gfx] flush ok");
    return;
  }

  // QSPI writePixels asserts when a single CS session spans >4096 pixels.
  // One row per addr window keeps each writePixels under that limit.
  auto* panel = static_cast<Arduino_TFT*>(gfx_);
  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  if (targetUsesPixelAlign2()) {
    for (int16_t row = 0; row + 2 <= h; row += 2) {
      uint16_t* row_ptr =
          const_cast<uint16_t*>(src + static_cast<size_t>(row) * static_cast<size_t>(w));
      panel->writeAddrWindow(x, static_cast<int16_t>(y + row), static_cast<uint16_t>(w), 2);
      panel->writePixels(row_ptr, static_cast<uint32_t>(w) * 2U);
    }
  } else {
    for (int16_t row = 0; row < h; ++row) {
      uint16_t* row_ptr =
          const_cast<uint16_t*>(src + static_cast<uint16_t>(row) * static_cast<size_t>(w));
      panel->writeAddrWindow(x, static_cast<int16_t>(y + row), static_cast<uint16_t>(w), 1);
      panel->writePixels(row_ptr, static_cast<uint32_t>(w));
    }
  }
  if (opened_here) {
    endWrite();
  }

  hardware::gfxLog("[gfx] flush ok");
}

void PlaneGfx::draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t* bitmap,
                                  int16_t w, int16_t h) {
  if (gfx_ == nullptr || bitmap == nullptr) {
    return;
  }
  if (hardware_panel_) {
    blitRegionFromBuffer(x, y, w, h, bitmap, w);
    return;
  }
  gfx_->draw16bitRGBBitmap(x, y, const_cast<uint16_t*>(bitmap), w, h);
}

void PlaneGfx::draw16bitRGBBitmap(int16_t x, int16_t y, const uint16_t* bitmap,
                                  uint16_t transparent_color, int16_t w,
                                  int16_t h) {
  if (gfx_ == nullptr || bitmap == nullptr) {
    return;
  }
  if (!hardware_panel_) {
    gfx_->draw16bitRGBBitmap(x, y, const_cast<uint16_t*>(bitmap), transparent_color, w,
                             h);
    return;
  }
  const bool opened_here = write_depth_ == 0;
  if (opened_here) {
    startWrite();
  }
  blitRegionWithTransparentKey(x, y, w, h, bitmap, w, transparent_color, this);
  if (opened_here) {
    endWrite();
  }
}

void PlaneGfx::blitRegionFromBuffer(int16_t x, int16_t y, int16_t w, int16_t h,
                                    const uint16_t* src, int16_t src_stride) {
  if (gfx_ == nullptr || src == nullptr || w <= 0 || h <= 0 || src_stride <= 0) {
    return;
  }

  if (!hardware_panel_) {
    gfx_->draw16bitRGBBitmap(x, y, const_cast<uint16_t*>(src), w, h);
    return;
  }

  const int16_t screen_w = static_cast<int16_t>(gfx_->width());
  const int16_t screen_h = static_cast<int16_t>(gfx_->height());
  if (x >= screen_w || y >= screen_h) {
    return;
  }

  // [perf] flush: time panel blits of a third of the screen or more.
  struct FlushTimer {
    bool on;
    uint32_t t0;
    ~FlushTimer() {
      if (on) {
        gfx_perf::add(gfx_perf::Kind::Flush, static_cast<uint32_t>(micros()) - t0);
      }
    }
  } const flush_timer{static_cast<int32_t>(w) * h * 3 >=
                          static_cast<int32_t>(screen_w) * screen_h,
                      static_cast<uint32_t>(micros())};

  if (x < 0) {
    src += static_cast<size_t>(-x);
    w += x;
    x = 0;
  }
  if (y < 0) {
    src += static_cast<size_t>(-y) * static_cast<size_t>(src_stride);
    h += y;
    y = 0;
  }
  if (x + w > screen_w) {
    w = screen_w - x;
  }
  if (y + h > screen_h) {
    h = screen_h - y;
  }
  if (w <= 0 || h <= 0) {
    return;
  }

  if (x == 0 && y == 0 && w == screen_w && h == screen_h && src_stride == w) {
    panelFlushBitmap(x, y, w, h, src);
    return;
  }

  if (!targetUsesPixelAlign2()) {
    const bool opened_here = write_depth_ == 0;
    if (opened_here) {
      startWrite();
    }
    auto* panel = static_cast<Arduino_TFT*>(gfx_);
    for (int16_t row = 0; row < h; ++row) {
      const uint16_t* row_ptr =
          src + static_cast<size_t>(row) * static_cast<size_t>(src_stride);
      panel->writeAddrWindow(x, static_cast<int16_t>(y + row), static_cast<uint16_t>(w), 1);
      panel->writePixels(const_cast<uint16_t*>(row_ptr), static_cast<uint32_t>(w));
    }
    if (opened_here) {
      endWrite();
    }
    return;
  }

  const int16_t blit_x = x;
  const int16_t blit_y = y;
  if (!expandBlitAreaForPixelAlign2(&x, &y, &w, &h, screen_w, screen_h)) {
    return;
  }
  src += static_cast<size_t>(y - blit_y) * static_cast<size_t>(src_stride) +
         static_cast<size_t>(x - blit_x);

  const size_t pixels = static_cast<size_t>(w) * static_cast<size_t>(h);
  if (!ensureBlitScratch(pixels)) {
    return;
  }

  for (int16_t row = 0; row < h; ++row) {
    memcpy(s_blit_scratch + static_cast<size_t>(row) * static_cast<size_t>(w),
           src + static_cast<size_t>(row) * static_cast<size_t>(src_stride),
           static_cast<size_t>(w) * sizeof(uint16_t));
  }

  panelFlushBitmap(x, y, w, h, s_blit_scratch);
}

PlaneGfxSprite::PlaneGfxSprite(PlaneGfx* parent) : parent_(parent) {}

PlaneGfxSprite::~PlaneGfxSprite() { deleteSprite(); }

bool PlaneGfxSprite::createSprite(int16_t w, int16_t h) {
  deleteSprite();
  const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(uint16_t);
  buffer_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (buffer_ == nullptr) {
    buffer_ = static_cast<uint16_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
  }
  if (buffer_ == nullptr) {
    return false;
  }
  width_ = w;
  height_ = h;
  canvas_.attach(new SpriteCanvas(buffer_, w, h));
  canvas_.setFramebuffer(buffer_, w, h);
  canvas_.setTextWrap(false);
  return true;
}

void PlaneGfxSprite::deleteSprite() {
  if (canvas_.raw() != nullptr) {
    delete canvas_.raw();
    canvas_.attach(nullptr);
  }
  canvas_.setFramebuffer(nullptr, 0, 0);
  if (buffer_ != nullptr) {
    heap_caps_free(buffer_);
    buffer_ = nullptr;
  }
  width_ = 0;
  height_ = 0;
}

void PlaneGfxSprite::pushSprite(int16_t x, int16_t y) {
  if (parent_ == nullptr || buffer_ == nullptr || parent_->raw() == nullptr) {
    return;
  }
  parent_->draw16bitRGBBitmap(x, y, buffer_, width_, height_);
}

void PlaneGfxSprite::pushSprite(int16_t x, int16_t y, uint16_t transparent_color) {
  if (parent_ == nullptr || buffer_ == nullptr || parent_->raw() == nullptr) {
    return;
  }
  parent_->draw16bitRGBBitmap(x, y, buffer_, transparent_color, width_, height_);
}
