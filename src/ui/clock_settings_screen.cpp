#include "ui/clock_settings_screen.h"

#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/clock_time.h"
#include "ui/radar_theme.h"
#include "ui/settings_rows.h"
#include "ui/ui_scale.h"

namespace ui {
namespace {

constexpr int kBezelInsetPx = ui::sc(10);
constexpr int kTitleGap = ui::sc(6);
constexpr int kSectionGap = ui::sc(8);

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;

enum class ClockSettingsRow : uint8_t { Timezone, Format, DateFormat };
constexpr int kRowCount = 3;

ClockSettingsRow s_focus = ClockSettingsRow::Timezone;

// Row bands from the last draw, for tap-to-select / tap-a-side-to-step.
int s_rows_top = 0;
int s_row_pitch = 0;

void buildStrings(char* tz_line, size_t tz_len, char* fmt_line, size_t fmt_len,
                  char* date_line, size_t date_len) {
  char tz_label[40];
  services::clock::formatTimezoneLabel(tz_label, sizeof(tz_label));
  // formatTimezoneLabel gives "Auto: New York", "Auto..." or "UTC-5". Show
  // "Zone: New York (auto)": shorter, and no double colon.
  if (strncmp(tz_label, "Auto: ", 6) == 0) {
    snprintf(tz_line, tz_len, "Zone: %s (auto)", tz_label + 6);
  } else if (strncmp(tz_label, "Auto", 4) == 0) {
    snprintf(tz_line, tz_len, "Zone: auto");
  } else {
    snprintf(tz_line, tz_len, "Zone: %s", tz_label);
  }
  snprintf(fmt_line, fmt_len, "Time format: %s",
           services::clock::use24Hour() ? "24 hour" : "12 hour");
  snprintf(date_line, date_len, "Date format: %s",
           services::clock::useNumericDate() ? "numeric" : "text");
}

}  // namespace

int8_t clockSettingsTouch(int16_t x, int16_t y) {
#if !FLIGHTSCNR_HAS_ENCODER
  if (s_row_pitch <= 0) {
    return 0;
  }
  const int slack = ui::sc(8);
  if (y < s_rows_top - slack || y >= s_rows_top + kRowCount * s_row_pitch + slack) {
    return 0;
  }
  int hit = (y - s_rows_top) / s_row_pitch;
  if (y < s_rows_top) {
    hit = 0;
  }
  if (hit >= kRowCount) {
    hit = kRowCount - 1;
  }
  if (hit != static_cast<int>(s_focus)) {
    s_focus = static_cast<ClockSettingsRow>(hit);
    return 0;
  }
  // The selected row has an arrow at each end: left half steps back, right forward.
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
  const settings_rows::Colors colors = settings_rows::defaultColors();
  const uint16_t bg = colors.bg;
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t hint_fg = tft.color565(120, 140, 160);

  char lines[kRowCount][48];
  buildStrings(lines[0], sizeof(lines[0]), lines[1], sizeof(lines[1]), lines[2],
               sizeof(lines[2]));

  const UiTextStyle row_style = displayFontBody();
  const UiTextStyle hint_style = displayFontScale();
  const int title_h = displayFontHeight(tft, displayFontTitle());
  const int pitch = settings_rows::rowPitch(row_style);
  const int hint_h = displayFontHeight(tft, hint_style);
  const int block_h = title_h + kTitleGap + kRowCount * pitch + kSectionGap + hint_h;

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

  s_rows_top = y;
  s_row_pitch = pitch;
  for (int i = 0; i < kRowCount; ++i) {
    settings_rows::drawOptionRow(lines[i], y, pitch, row_style,
                                 i == static_cast<int>(s_focus), colors);
    y += pitch;
  }

  y += kSectionGap;
  displayFontApply(tft, hint_style);
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(hint_fg, bg);
  tft.drawString("Swipe right: back", kCenterX, y);

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
