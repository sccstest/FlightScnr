#include "ui/clock_settings_screen.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/clock_time.h"
#include "ui/radar_accent.h"
#include "ui/radar_theme.h"
#include "ui/ui_scale.h"

namespace ui {
namespace {

constexpr int kBezelInsetPx = ui::sc(10);
constexpr int kTextPadPx = ui::sc(6);
constexpr int kTitleGap = ui::sc(6);
constexpr int kLineGap = ui::sc(4);

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;
const int kCircleRadius = kCenterX - kBezelInsetPx;

enum class ClockSettingsRow : uint8_t { Timezone, Format, DateFormat };

ClockSettingsRow s_focus = ClockSettingsRow::Timezone;

#if !FLIGHTSCNR_HAS_ENCODER
// Touch-only boards: row bounds from the last draw, for tap-to-select / tap-to-step.
int s_row_top[3] = {};
int s_row_bottom[3] = {};
int s_row_count = 0;
#endif

const char* focusDecorate(const char* text, bool focused, char* buf, size_t len) {
#if !FLIGHTSCNR_HAS_ENCODER
  if (focused) {
    snprintf(buf, len, "<  %s  >", text);
    return buf;
  }
#else
  (void)focused;
  (void)buf;
  (void)len;
#endif
  return text;
}

int circleHalfWidthAtRow(int row_y, int row_h) {
  if (kCircleRadius <= 0 || row_h <= 0) {
    return 0;
  }
  const int row_center_y = row_y + row_h / 2;
  const int dy = row_center_y - kCenterY;
  if (std::abs(dy) >= kCircleRadius) {
    return 0;
  }
  const float half =
      std::sqrt(static_cast<float>(kCircleRadius * kCircleRadius - dy * dy));
  const int usable = static_cast<int>(half) - kTextPadPx;
  return usable > 0 ? usable : 0;
}

void fitLineToWidth(char* text, size_t len, UiTextStyle style, int max_width_px) {
  if (max_width_px <= 0 || text[0] == '\0') {
    return;
  }
  displayFontApply(tft, style);
  if (tft.textWidth(text) <= max_width_px) {
    return;
  }
  const size_t raw_len = strlen(text);
  for (size_t n = raw_len; n > 0; --n) {
    snprintf(text, len, "%.*s…", static_cast<int>(n), text);
    if (tft.textWidth(text) <= max_width_px) {
      return;
    }
  }
  strncpy(text, "…", len);
  text[len - 1] = '\0';
}

void drawCenterLine(const char* text, int* y, UiTextStyle style, uint16_t fg,
                    uint16_t bg) {
  displayFontApply(tft, style);
  const int h = displayFontHeight(tft, style);
  const int max_half_w = circleHalfWidthAtRow(*y, h);
  const int max_w = max_half_w * 2;

  char line[56];
  strncpy(line, text, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';
  fitLineToWidth(line, sizeof(line), style, max_w);

  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString(line, kCenterX, *y);
  *y += h + kLineGap;
}

struct InfoLine {
  const char* text;
  UiTextStyle style;
  uint16_t color;
};

int measureBlockHeight(const InfoLine* lines, size_t count) {
  int total = 0;
  for (size_t i = 0; i < count; ++i) {
    total += displayFontHeight(tft, lines[i].style);
    if (i + 1 < count) {
      total += kLineGap;
    }
  }
  return total;
}

void buildStrings(char* tz_line, size_t tz_len, char* fmt_line, size_t fmt_len,
                  char* date_line, size_t date_len) {
  char tz_label[16];
  services::clock::formatTimezoneLabel(tz_label, sizeof(tz_label));
  snprintf(tz_line, tz_len, "Timezone: %s", tz_label);
  snprintf(fmt_line, fmt_len, "Time format: %s",
           services::clock::use24Hour() ? "24 hour" : "12 hour");
  snprintf(date_line, date_len, "Date format: %s",
           services::clock::useNumericDate() ? "numeric" : "text");
}

}  // namespace

int8_t clockSettingsTouch(int16_t x, int16_t y) {
#if !FLIGHTSCNR_HAS_ENCODER
  int hit = -1;
  for (int i = 0; i < s_row_count; ++i) {
    const int top = (i == 0) ? s_row_top[i] - ui::sc(6) : s_row_top[i];
    const int bottom = (i + 1 < s_row_count) ? s_row_top[i + 1] : s_row_bottom[i] + ui::sc(6);
    if (y >= top && y < bottom) {
      hit = i;
      break;
    }
  }
  if (hit < 0) {
    return 0;
  }
  if (hit != static_cast<int>(s_focus)) {
    s_focus = static_cast<ClockSettingsRow>(hit);
    return 0;
  }
  return x < kCenterX ? -1 : 1;
#else
  (void)x;
  (void)y;
  return 0;
#endif
}

void clockSettingsResetFocus() { s_focus = ClockSettingsRow::Timezone; }

void clockSettingsCycleFocus() {
  switch (s_focus) {
    case ClockSettingsRow::Timezone:
      s_focus = ClockSettingsRow::Format;
      break;
    case ClockSettingsRow::Format:
      s_focus = ClockSettingsRow::DateFormat;
      break;
    case ClockSettingsRow::DateFormat:
      s_focus = ClockSettingsRow::Timezone;
      break;
  }
}

void clockSettingsScreenDraw() {
  tft.beginOffscreen();
  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t label_fg = tft.color565(180, 200, 220);
  uint8_t accent_r = 0;
  uint8_t accent_g = 0;
  uint8_t accent_b = 0;
  radar::accentHighlightRgb(&accent_r, &accent_g, &accent_b);
  const uint16_t active_fg = tft.color565(accent_r, accent_g, accent_b);

  char tz_line[32];
  char fmt_line[28];
  char date_line[28];
  buildStrings(tz_line, sizeof(tz_line), fmt_line, sizeof(fmt_line), date_line,
               sizeof(date_line));

  const uint16_t tz_fg =
      (s_focus == ClockSettingsRow::Timezone) ? active_fg : label_fg;
  const uint16_t fmt_fg =
      (s_focus == ClockSettingsRow::Format) ? active_fg : label_fg;
  const uint16_t date_fg =
      (s_focus == ClockSettingsRow::DateFormat) ? active_fg : label_fg;

  const int title_h = displayFontHeight(tft, displayFontTitle());
  char deco[3][48];
  const InfoLine option_lines[] = {
      {focusDecorate(tz_line, s_focus == ClockSettingsRow::Timezone, deco[0], 48),
       displayFontBody(), tz_fg},
      {focusDecorate(fmt_line, s_focus == ClockSettingsRow::Format, deco[1], 48),
       displayFontBody(), fmt_fg},
      {focusDecorate(date_line, s_focus == ClockSettingsRow::DateFormat, deco[2], 48),
       displayFontBody(), date_fg},
  };
  const int options_h = measureBlockHeight(option_lines, sizeof(option_lines) / sizeof(option_lines[0]));
  const int block_h = title_h + kTitleGap + options_h;

  tft.fillScreen(bg);

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  displayFontApply(tft, displayFontTitle());
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString("Clock settings", kCenterX, y);
  y += title_h + kTitleGap;

#if !FLIGHTSCNR_HAS_ENCODER
  s_row_count = 0;
#endif
  for (const InfoLine& line : option_lines) {
#if !FLIGHTSCNR_HAS_ENCODER
    const int top = y;
#endif
    drawCenterLine(line.text, &y, line.style, line.color, bg);
#if !FLIGHTSCNR_HAS_ENCODER
    if (s_row_count < 3) {
      s_row_top[s_row_count] = top;
      s_row_bottom[s_row_count] = y;
      ++s_row_count;
    }
#endif
  }

  tft.setTextDatum(TextDatum::TopLeft);
  tft.endOffscreen();
}

void clockSettingsHandleKnob(int8_t delta) {
  if (delta == 0) {
    return;
  }

  switch (s_focus) {
    case ClockSettingsRow::Timezone:
      services::clock::stepTimezoneHours(delta);
      break;
    case ClockSettingsRow::Format:
      services::clock::toggleHourFormat();
      break;
    case ClockSettingsRow::DateFormat:
      services::clock::toggleDateFormat();
      break;
  }
  clockSettingsScreenDraw();
}

}  // namespace ui
