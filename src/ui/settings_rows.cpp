#include "ui/settings_rows.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/pin_config.h"
#include "ui/radar_accent.h"
#include "ui/ui_scale.h"

namespace ui {
namespace settings_rows {
namespace {

constexpr int kBezelInsetPx = ui::sc(10);
constexpr int kTextPadPx = ui::sc(6);
const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;
const int kCircleRadius = kCenterX - kBezelInsetPx;

#if !FLIGHTSCNR_HAS_ENCODER
/** Gap between touch rows: font height + this is about 70 px on the 4C. */
constexpr int kTouchRowGapPx = ui::sc(16);
/** Arrow half-height and its distance from the pill end. */
constexpr int kArrowHalfPx = ui::sc(6);
constexpr int kArrowInsetPx = ui::sc(9);
constexpr int kArrowTextGapPx = ui::sc(5);
#endif

}  // namespace

int rowPitch(UiTextStyle style) {
  const int h = displayFontHeight(tft, style);
#if !FLIGHTSCNR_HAS_ENCODER
  return h + kTouchRowGapPx;
#else
  return h + ui::sc(4);
#endif
}

int halfWidthForBand(int top, int h) {
  if (kCircleRadius <= 0 || h <= 0) {
    return 0;
  }
  // The band edge farther from the center is the narrow one.
  const int dy_top = std::abs(top - kCenterY);
  const int dy_bottom = std::abs(top + h - kCenterY);
  const int dy = dy_top > dy_bottom ? dy_top : dy_bottom;
  if (dy >= kCircleRadius) {
    return 0;
  }
  const float half =
      std::sqrt(static_cast<float>(kCircleRadius * kCircleRadius - dy * dy));
  const int usable = static_cast<int>(half) - kTextPadPx;
  return usable > 0 ? usable : 0;
}

void fitText(char* text, size_t len, UiTextStyle style, int max_width_px) {
  if (max_width_px <= 0 || text == nullptr || text[0] == '\0') {
    return;
  }
  displayFontApply(tft, style);
  if (tft.textWidth(text) <= max_width_px) {
    return;
  }
  char original[64];
  strncpy(original, text, sizeof(original) - 1);
  original[sizeof(original) - 1] = '\0';
  for (size_t n = strlen(original); n > 0; --n) {
    snprintf(text, len, "%.*s...", static_cast<int>(n), original);
    if (tft.textWidth(text) <= max_width_px) {
      return;
    }
  }
  strncpy(text, "...", len);
  text[len - 1] = '\0';
}

void drawOptionRow(const char* text, int top, int pitch, UiTextStyle style, bool focused,
                   const Colors& colors) {
  const int font_h = displayFontHeight(tft, style);
  // Pill: the row band minus a small gap so neighbouring pills never touch.
  const int pill_h = pitch - ui::sc(4) > font_h ? pitch - ui::sc(4) : font_h;
  const int cy = top + pitch / 2;
  const int pill_top = cy - pill_h / 2;
  const int half = halfWidthForBand(pill_top, pill_h);
  if (half <= 0) {
    return;
  }

  int text_half = half;
#if !FLIGHTSCNR_HAS_ENCODER
  // Leave room for the arrows on every row so text never jumps when selected.
  text_half = half - (kArrowInsetPx + 2 * kArrowHalfPx + kArrowTextGapPx);
  if (text_half < half / 2) {
    text_half = half / 2;
  }
#endif

  const uint16_t text_bg = focused ? colors.focus_fill : colors.bg;
  if (focused) {
    const int r = pill_h / 2;
    const int left = kCenterX - half;
    const int width = 2 * half;
    if (width > 2 * r) {
      tft.fillRect(static_cast<int16_t>(left + r), static_cast<int16_t>(pill_top),
                   static_cast<int16_t>(width - 2 * r), static_cast<int16_t>(pill_h),
                   colors.focus_fill);
    }
    tft.fillCircle(static_cast<int16_t>(left + r), static_cast<int16_t>(cy),
                   static_cast<int16_t>(r), colors.focus_fill);
    tft.fillCircle(static_cast<int16_t>(left + width - r), static_cast<int16_t>(cy),
                   static_cast<int16_t>(r), colors.focus_fill);
#if !FLIGHTSCNR_HAS_ENCODER
    // Arrows mark the two tap zones: left half = back, right half = forward.
    const int lx = left + kArrowInsetPx;
    tft.fillTriangle(static_cast<int16_t>(lx), static_cast<int16_t>(cy),
                     static_cast<int16_t>(lx + 2 * kArrowHalfPx),
                     static_cast<int16_t>(cy - kArrowHalfPx - ui::sc(2)),
                     static_cast<int16_t>(lx + 2 * kArrowHalfPx),
                     static_cast<int16_t>(cy + kArrowHalfPx + ui::sc(2)), colors.focus_text);
    const int rx = left + width - kArrowInsetPx;
    tft.fillTriangle(static_cast<int16_t>(rx), static_cast<int16_t>(cy),
                     static_cast<int16_t>(rx - 2 * kArrowHalfPx),
                     static_cast<int16_t>(cy - kArrowHalfPx - ui::sc(2)),
                     static_cast<int16_t>(rx - 2 * kArrowHalfPx),
                     static_cast<int16_t>(cy + kArrowHalfPx + ui::sc(2)), colors.focus_text);
#endif
  }

  char line[64];
  strncpy(line, text, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';
  // A long label on a narrow (top or bottom) row drops to the detail font before
  // anything is cut: a truncated value ("Compass rose: ...") hides the setting.
  UiTextStyle use = style;
  displayFontApply(tft, use);
  if (tft.textWidth(line) > 2 * text_half) {
    use = displayFontDetail();
  }
  fitText(line, sizeof(line), use, 2 * text_half);
  displayFontApply(tft, use);
  const int use_h = displayFontHeight(tft, use);
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(focused ? colors.focus_text : colors.text, text_bg);
  tft.drawString(line, static_cast<int16_t>(kCenterX), static_cast<int16_t>(cy - use_h / 2));
}

Colors defaultColors() {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  radar::accentHighlightRgb(&r, &g, &b);
  Colors c;
  c.text = tft.color565(180, 200, 220);
  c.focus_text = tft.color565(r, g, b);
  c.focus_fill = tft.color565(34, 46, 62);
  c.bg = tft.color565(0, 0, 0);
  return c;
}

}  // namespace settings_rows
}  // namespace ui
