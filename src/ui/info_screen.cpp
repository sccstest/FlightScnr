#include "ui/info_screen.h"

#include <WiFi.h>

#include <cmath>
#include <cstdio>
#include <cstring>

#include "config.h"
#include "hardware/buzzer.h"
#include "hardware/display.h"
#include "hardware/display_brightness.h"
#include "hardware/display_font.h"
#include "hardware/pin_config.h"
#include "services/adsb_client.h"
#include "services/aircraft_alert.h"
#include "services/api_keys.h"
#include "services/device_identity.h"
#include "services/map_center.h"
#include "services/wifi_setup.h"
#include "ui/display_prefs.h"
#include "ui/radar_accent.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"
#include "ui/ui_scale.h"

namespace {

constexpr int kBezelInsetPx = ui::sc(10);
constexpr int kTextPadPx = ui::sc(6);
constexpr int kTitleGap = ui::sc(6);
constexpr int kLineGap = ui::sc(4);
constexpr int kSectionGap = ui::sc(6);

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;
const int kCircleRadius = kCenterX - kBezelInsetPx;

ui::InfoSettingsPage s_page = ui::InfoSettingsPage::Main;

enum class DisplayAdjustRow : uint8_t {
  Brightness,
  Units,
  Range,
  Compass,
  Rings,
  Facing,
};

DisplayAdjustRow s_display_focus = DisplayAdjustRow::Brightness;

// Page 3/3: radar behavior timeouts, sweep, color, and audio.
enum class ColorsAdjustRow : uint8_t {
  Sweep,
  BlipDetails,
  DetailTimeout,
  ClockTimeout,
  IdleClock,
  Color,
  BeepOn,
#if !FLIGHTSCNR_HAS_HAPTIC
  BeepTone,
#endif
};

ColorsAdjustRow s_colors_focus = ColorsAdjustRow::Sweep;

// Page 4/4: quick-glance type colors and per-class alerts.
enum class AlertsAdjustRow : uint8_t {
  TypeColors,
  Military,
  Emergency,
  Helicopters,
  Private,
  When,
  OverheadAny,
  Count,
};

AlertsAdjustRow s_alerts_focus = AlertsAdjustRow::TypeColors;

#if !FLIGHTSCNR_HAS_ENCODER
// Touch-only boards (Waveshare 4C): remember where each option row was drawn so a
// tap can select a row, and the selected row's left/right half can step its value.
constexpr int kMaxTouchRows = 10;
int s_row_top[kMaxTouchRows] = {};
int s_row_bottom[kMaxTouchRows] = {};
int s_row_count = 0;
#endif

#if !FLIGHTSCNR_HAS_KNOB_BUTTON
// No knob button to hold for a Wi-Fi wipe: page 1 gets a "Reset Wi-Fi" row
// instead. First tap arms it ("Tap again to reset"), a second tap within
// kWifiResetConfirmMs wipes saved networks and reboots into the setup portal.
constexpr unsigned long kWifiResetConfirmMs = 5000UL;
unsigned long s_wifi_reset_armed_ms = 0;
int s_wifi_reset_row_top = -1;
int s_wifi_reset_row_bottom = -1;
#endif

/** Selected rows get "<  ... >" on touch-only boards: tap a side to change it. */
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

  char line[64];
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

void formatRouteApiLine(const char* name, bool enabled, bool has_key, bool can_use,
                        char* out, size_t out_len) {
  if (!enabled) {
    snprintf(out, out_len, "%s: off", name);
  } else if (!has_key) {
    snprintf(out, out_len, "%s: no key", name);
  } else if (!can_use) {
    snprintf(out, out_len, "%s: limit", name);
  } else {
    snprintf(out, out_len, "%s: active", name);
  }
}

void buildApiStatusStrings(char* airlabs_line, size_t airlabs_len, char* fa_line,
                           size_t fa_len, char* fr24_line, size_t fr24_len,
                           char* adsbdb_line, size_t adsbdb_len) {
  services::apikeys::load();
  formatRouteApiLine("AirLabs", services::apikeys::useAirLabs(),
                     services::apikeys::hasAirLabs(), services::apikeys::canUseAirLabs(),
                     airlabs_line, airlabs_len);
  formatRouteApiLine("FlightAware", services::apikeys::useFlightAware(),
                     services::apikeys::hasFlightAware(),
                     services::apikeys::canUseFlightAware(), fa_line, fa_len);
  formatRouteApiLine("FR24", services::apikeys::useFr24(), services::apikeys::hasFr24(),
                     services::apikeys::canUseFr24(), fr24_line, fr24_len);
  // adsbdb is key-less: simple on/off status (no key/limit states). Naming
  // the source here doubles as the ToS-requested route-data attribution.
  snprintf(adsbdb_line, adsbdb_len, "adsbdb.com: %s",
           services::apikeys::useAdsbDb() ? "on" : "off");
}

void buildMainStrings(char* ip_line, size_t ip_len, char* wifi_line, size_t wifi_len,
                      char* saved_line, size_t saved_len, char* lat_line, size_t lat_len,
                      char* lon_line, size_t lon_len, char* alt_line, size_t alt_len,
                      char* web_line, size_t web_len) {
  if (WiFi.status() == WL_CONNECTED) {
    snprintf(ip_line, ip_len, "IP: %s", WiFi.localIP().toString().c_str());
    snprintf(wifi_line, wifi_len, "Wi-Fi: %s", WiFi.SSID().c_str());
  } else {
    snprintf(ip_line, ip_len, "IP: Not connected");
    snprintf(wifi_line, wifi_len, "Wi-Fi: —");
  }
  snprintf(saved_line, saved_len, "Saved: %u/%u",
           static_cast<unsigned>(wifiNetsCount()),
           static_cast<unsigned>(config::kWifiMaxNetworks));

  snprintf(lat_line, lat_len, "Lat: %.5f", services::map_center::latitude());
  snprintf(lon_line, lon_len, "Lon: %.5f", services::map_center::longitude());

  const int min_ft = services::adsb::altitudeFloorFt();
  if (min_ft > 0) {
    snprintf(alt_line, alt_len, "Min alt: %d ft", min_ft);
  } else {
    snprintf(alt_line, alt_len, "Min alt: off");
  }

  if (WiFi.status() == WL_CONNECTED) {
    snprintf(web_line, web_len, "Web: %s.local",
             services::device_identity::portalHostname());
  } else {
    snprintf(web_line, web_len, "Web: —");
  }
}

void buildDisplayStrings(char* bright_line, size_t bright_len, char* units_line,
                         size_t units_len, char* range_line, size_t range_len,
                         char* compass_line, size_t compass_len, char* facing_line,
                         size_t facing_len) {
  snprintf(bright_line, bright_len, "Brightness: %u%%",
           static_cast<unsigned>(hardware::displayBrightnessPercent()));
  snprintf(units_line, units_len, "Units: %s", ui::radar::distanceUnitLabel());
  char range_tag[12];
  ui::radar::formatActiveScaleTag(range_tag, sizeof(range_tag));
  snprintf(range_line, range_len, "Range: %s", range_tag);
  snprintf(compass_line, compass_len, "Compass Rose: %s",
           ui::radar::showCompassRose() ? "on" : "off");
  char facing_tag[12];
  ui::radar::facingLabel(facing_tag, sizeof(facing_tag));
  snprintf(facing_line, facing_len, "Facing: %s", facing_tag);
}

void buildColorsStrings(char* sweep_line, size_t sweep_len, char* blip_line,
                        size_t blip_len, char* detail_line, size_t detail_len,
                        char* clock_line, size_t clock_len, char* idle_line,
                        size_t idle_len, char* color_line, size_t color_len,
                        char* beep_line, size_t beep_len
#if !FLIGHTSCNR_HAS_HAPTIC
                        ,
                        char* beep_tone_line, size_t beep_tone_len
#endif
) {
  snprintf(sweep_line, sweep_len, "Radar Sweep: %s",
           ui::displayPrefsSweepLineEnabled() ? "on" : "off");
  snprintf(blip_line, blip_len, "Hide Blip Details: %s",
           ui::displayPrefsHideBlipDetails() ? "on" : "off");
  snprintf(detail_line, detail_len, "Flight Detail: %s",
           ui::displayPrefsFlightDetailTimeoutLabel());
  snprintf(clock_line, clock_len, "Clock/Forecast: %s",
           ui::displayPrefsClockWeatherTimeoutLabel());
  snprintf(idle_line, idle_len, "Idle Clock: %s",
           ui::displayPrefsAutoIdleClockEnabled() ? "on" : "off");
  snprintf(color_line, color_len, "Radar color: %s", ui::radar::accentColorName());
#if FLIGHTSCNR_HAS_HAPTIC
  snprintf(beep_line, beep_len, "Vibration: %s",
           hardware::buzzerEnabled() ? "on" : "off");
#elif FLIGHTSCNR_HAS_SPEAKER
  snprintf(beep_line, beep_len, "Alert Sound: %s",
           hardware::buzzerEnabled() ? "on" : "off");
  snprintf(beep_tone_line, beep_tone_len, "Chime Volume: %c",
           hardware::buzzerToneLetter());
#else
  snprintf(beep_line, beep_len, "UI Beep: %s",
           hardware::buzzerEnabled() ? "on" : "off");
  snprintf(beep_tone_line, beep_tone_len, "Beep Tone: %c",
           hardware::buzzerToneLetter());
#endif
}

void drawMainPage(uint16_t bg, uint16_t fg, uint16_t label_fg, uint16_t hint_fg) {
  char ip_line[40];
  char wifi_line[40];
  char saved_line[24];
  char lat_line[24];
  char lon_line[24];
  char alt_line[24];
  char web_line[36];
  char airlabs_line[24];
  char fa_line[28];
  char fr24_line[20];
  char adsbdb_line[20];

  buildMainStrings(ip_line, sizeof(ip_line), wifi_line, sizeof(wifi_line), saved_line,
                   sizeof(saved_line), lat_line, sizeof(lat_line), lon_line, sizeof(lon_line),
                   alt_line, sizeof(alt_line), web_line, sizeof(web_line));
  buildApiStatusStrings(airlabs_line, sizeof(airlabs_line), fa_line, sizeof(fa_line),
                        fr24_line, sizeof(fr24_line), adsbdb_line, sizeof(adsbdb_line));

  const int title_h = displayFontHeight(tft, displayFontTitle());
  const InfoLine main_lines[] = {
      {ip_line, displayFontBody(), fg},
      {wifi_line, displayFontBody(), fg},
      {saved_line, displayFontDetail(), label_fg},
      {lat_line, displayFontDetail(), label_fg},
      {lon_line, displayFontDetail(), label_fg},
      {alt_line, displayFontDetail(), label_fg},
      {web_line, displayFontDetail(), hint_fg},
  };
  const InfoLine api_lines[] = {
      {"Route APIs", displayFontDetail(), hint_fg},
      {airlabs_line, displayFontDetail(), label_fg},
      {fa_line, displayFontDetail(), label_fg},
      {fr24_line, displayFontDetail(), label_fg},
      {adsbdb_line, displayFontDetail(), label_fg},
  };
  const int main_h = measureBlockHeight(main_lines, sizeof(main_lines) / sizeof(main_lines[0]));
  const int api_h = measureBlockHeight(api_lines, sizeof(api_lines) / sizeof(api_lines[0]));
  int block_h = title_h + kTitleGap + main_h + kSectionGap + api_h;
#if !FLIGHTSCNR_HAS_KNOB_BUTTON
  block_h += kSectionGap + displayFontHeight(tft, displayFontDetail());
#endif

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  displayFontApply(tft, displayFontTitle());
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString("Settings 1/4", kCenterX, y);
  y += title_h + kTitleGap;

  for (const InfoLine& line : main_lines) {
    drawCenterLine(line.text, &y, line.style, line.color, bg);
  }

  y += kSectionGap;

  auto api_color = [&](bool enabled, bool has_key, bool can_use) -> uint16_t {
    return (enabled && has_key && can_use) ? fg : label_fg;
  };

  drawCenterLine("Route APIs", &y, displayFontDetail(), hint_fg, bg);
  drawCenterLine(airlabs_line, &y, displayFontDetail(),
                 api_color(services::apikeys::useAirLabs(), services::apikeys::hasAirLabs(),
                           services::apikeys::canUseAirLabs()),
                 bg);
  drawCenterLine(fa_line, &y, displayFontDetail(),
                 api_color(services::apikeys::useFlightAware(),
                           services::apikeys::hasFlightAware(),
                           services::apikeys::canUseFlightAware()),
                 bg);
  drawCenterLine(fr24_line, &y, displayFontDetail(),
                 api_color(services::apikeys::useFr24(), services::apikeys::hasFr24(),
                           services::apikeys::canUseFr24()),
                 bg);
  drawCenterLine(adsbdb_line, &y, displayFontDetail(),
                 services::apikeys::useAdsbDb() ? fg : label_fg, bg);

#if !FLIGHTSCNR_HAS_KNOB_BUTTON
  y += kSectionGap;
  const bool armed = s_wifi_reset_armed_ms != 0;
  s_wifi_reset_row_top = y;
  drawCenterLine(armed ? "Tap again to reset" : "[ Reset Wi-Fi ]", &y, displayFontDetail(),
                 armed ? tft.color565(255, 90, 80) : label_fg, bg);
  s_wifi_reset_row_bottom = y;
#endif
}

uint16_t settingsActiveFg() {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  ui::radar::accentHighlightRgb(&r, &g, &b);
  return tft.color565(r, g, b);
}

void drawDisplayPage(uint16_t bg, uint16_t fg, uint16_t label_fg) {
  char bright_line[32];
  char units_line[24];
  char range_line[24];
  char compass_line[28];
  char facing_line[24];
  buildDisplayStrings(bright_line, sizeof(bright_line), units_line, sizeof(units_line),
                      range_line, sizeof(range_line), compass_line, sizeof(compass_line),
                      facing_line, sizeof(facing_line));

  const uint16_t active_fg = settingsActiveFg();
  const uint16_t bright_fg =
      (s_display_focus == DisplayAdjustRow::Brightness) ? active_fg : label_fg;
  const uint16_t units_fg =
      (s_display_focus == DisplayAdjustRow::Units) ? active_fg : label_fg;
  const uint16_t range_fg =
      (s_display_focus == DisplayAdjustRow::Range) ? active_fg : label_fg;
  const uint16_t compass_fg =
      (s_display_focus == DisplayAdjustRow::Compass) ? active_fg : label_fg;
  const uint16_t facing_fg =
      (s_display_focus == DisplayAdjustRow::Facing) ? active_fg : label_fg;

  const int title_h = displayFontHeight(tft, displayFontTitle());
  char deco[6][48];
  char rings_line[28];
  snprintf(rings_line, sizeof(rings_line), "Rings: %s", ui::radar::gridModeLabel());
  const InfoLine option_lines[] = {
      {focusDecorate(bright_line, s_display_focus == DisplayAdjustRow::Brightness, deco[0], 48),
       displayFontBody(), bright_fg},
      {focusDecorate(units_line, s_display_focus == DisplayAdjustRow::Units, deco[1], 48),
       displayFontBody(), units_fg},
      {focusDecorate(range_line, s_display_focus == DisplayAdjustRow::Range, deco[2], 48),
       displayFontBody(), range_fg},
      {focusDecorate(compass_line, s_display_focus == DisplayAdjustRow::Compass, deco[3], 48),
       displayFontBody(), compass_fg},
      {focusDecorate(rings_line, s_display_focus == DisplayAdjustRow::Rings, deco[5], 48),
       displayFontBody(),
       s_display_focus == DisplayAdjustRow::Rings ? active_fg : label_fg},
      {focusDecorate(facing_line, s_display_focus == DisplayAdjustRow::Facing, deco[4], 48),
       displayFontBody(), facing_fg},
  };
  const int options_h = measureBlockHeight(option_lines, sizeof(option_lines) / sizeof(option_lines[0]));
  const int block_h = title_h + kTitleGap + options_h;

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  displayFontApply(tft, displayFontTitle());
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString("Settings 2/4", kCenterX, y);
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
    if (s_row_count < kMaxTouchRows) {
      s_row_top[s_row_count] = top;
      s_row_bottom[s_row_count] = y;
      ++s_row_count;
    }
#endif
  }
}

void drawColorsPage(uint16_t bg, uint16_t fg, uint16_t label_fg) {
  char sweep_line[24];
  char blip_line[28];
  char detail_line[28];
  char clock_line[28];
  char idle_line[24];
  char color_line[28];
  char beep_line[24];
#if !FLIGHTSCNR_HAS_HAPTIC
  char beep_tone_line[28];
#endif
  buildColorsStrings(sweep_line, sizeof(sweep_line), blip_line, sizeof(blip_line),
                     detail_line, sizeof(detail_line), clock_line, sizeof(clock_line),
                     idle_line, sizeof(idle_line), color_line, sizeof(color_line),
                     beep_line, sizeof(beep_line)
#if !FLIGHTSCNR_HAS_HAPTIC
                     ,
                     beep_tone_line, sizeof(beep_tone_line)
#endif
  );

  const uint16_t active_fg = settingsActiveFg();
  const uint16_t sweep_fg =
      (s_colors_focus == ColorsAdjustRow::Sweep) ? active_fg : label_fg;
  const uint16_t blip_fg =
      (s_colors_focus == ColorsAdjustRow::BlipDetails) ? active_fg : label_fg;
  const uint16_t detail_fg =
      (s_colors_focus == ColorsAdjustRow::DetailTimeout) ? active_fg : label_fg;
  const uint16_t clock_fg =
      (s_colors_focus == ColorsAdjustRow::ClockTimeout) ? active_fg : label_fg;
  const uint16_t idle_fg =
      (s_colors_focus == ColorsAdjustRow::IdleClock) ? active_fg : label_fg;
  const uint16_t color_fg =
      (s_colors_focus == ColorsAdjustRow::Color) ? active_fg : label_fg;
  const uint16_t beep_fg =
      (s_colors_focus == ColorsAdjustRow::BeepOn) ? active_fg : label_fg;
#if !FLIGHTSCNR_HAS_HAPTIC
  const uint16_t beep_tone_fg =
      (s_colors_focus == ColorsAdjustRow::BeepTone) ? active_fg : label_fg;
#endif

  const int title_h = displayFontHeight(tft, displayFontTitle());
  char deco[8][48];
  const ColorsAdjustRow f = s_colors_focus;
  const InfoLine option_lines[] = {
      {focusDecorate(sweep_line, f == ColorsAdjustRow::Sweep, deco[0], 48), displayFontBody(),
       sweep_fg},
      {focusDecorate(blip_line, f == ColorsAdjustRow::BlipDetails, deco[1], 48),
       displayFontBody(), blip_fg},
      {focusDecorate(detail_line, f == ColorsAdjustRow::DetailTimeout, deco[2], 48),
       displayFontBody(), detail_fg},
      {focusDecorate(clock_line, f == ColorsAdjustRow::ClockTimeout, deco[3], 48),
       displayFontBody(), clock_fg},
      {focusDecorate(idle_line, f == ColorsAdjustRow::IdleClock, deco[4], 48),
       displayFontBody(), idle_fg},
      {focusDecorate(color_line, f == ColorsAdjustRow::Color, deco[5], 48), displayFontBody(),
       color_fg},
      {focusDecorate(beep_line, f == ColorsAdjustRow::BeepOn, deco[6], 48), displayFontBody(),
       beep_fg},
#if !FLIGHTSCNR_HAS_HAPTIC
      {focusDecorate(beep_tone_line, f == ColorsAdjustRow::BeepTone, deco[7], 48),
       displayFontBody(), beep_tone_fg},
#endif
  };
  const int options_h = measureBlockHeight(option_lines, sizeof(option_lines) / sizeof(option_lines[0]));
  const int block_h = title_h + kTitleGap + options_h;

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  displayFontApply(tft, displayFontTitle());
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString("Settings 3/4", kCenterX, y);  y += title_h + kTitleGap;

#if !FLIGHTSCNR_HAS_ENCODER
  s_row_count = 0;
#endif
  for (const InfoLine& line : option_lines) {
#if !FLIGHTSCNR_HAS_ENCODER
    const int top = y;
#endif
    drawCenterLine(line.text, &y, line.style, line.color, bg);
#if !FLIGHTSCNR_HAS_ENCODER
    if (s_row_count < kMaxTouchRows) {
      s_row_top[s_row_count] = top;
      s_row_bottom[s_row_count] = y;
      ++s_row_count;
    }
#endif
  }
}

void drawAlertsPage(uint16_t bg, uint16_t fg, uint16_t label_fg) {
  const uint16_t active_fg = settingsActiveFg();
  auto onOff = [](bool v) { return v ? "on" : "off"; };
  constexpr int kRows = static_cast<int>(AlertsAdjustRow::Count);
  char rows[kRows][32];
  snprintf(rows[0], sizeof(rows[0]), "Type colors: %s",
           onOff(services::alert::typeColorsEnabled()));
  snprintf(rows[1], sizeof(rows[1]), "Military: %s",
           onOff(services::alert::militaryAlertEnabled()));
  snprintf(rows[2], sizeof(rows[2]), "Emergency: %s",
           onOff(services::alert::emergencyAlertEnabled()));
  snprintf(rows[3], sizeof(rows[3]), "Helicopters: %s",
           onOff(services::alert::helicopterAlertEnabled()));
  snprintf(rows[4], sizeof(rows[4]), "Private planes: %s",
           onOff(services::alert::privateAlertEnabled()));
  snprintf(rows[5], sizeof(rows[5]), "Alert when: %s",
           services::alert::alertOnApproachEnabled() ? "overhead" : "entering");
  snprintf(rows[6], sizeof(rows[6]), "Overhead (any): %s",
           onOff(services::alert::overheadAnyEnabled()));

  char deco[kRows][48];
  InfoLine option_lines[kRows];
  for (int i = 0; i < kRows; ++i) {
    const bool focused = static_cast<int>(s_alerts_focus) == i;
    option_lines[i] = {focusDecorate(rows[i], focused, deco[i], 48), displayFontBody(),
                       focused ? active_fg : label_fg};
  }
  // Legend for the type colors (same hues as the radar icons).
  const char* legend = "Red mil / Blue heli / White private";

  const int title_h = displayFontHeight(tft, displayFontTitle());
  const int legend_h = displayFontHeight(tft, displayFontDetail());
  const int options_h = measureBlockHeight(option_lines, kRows);
  const int block_h = title_h + kTitleGap + options_h + kSectionGap + legend_h;

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  displayFontApply(tft, displayFontTitle());
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString("Alerts 4/4", kCenterX, y);
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
    if (s_row_count < kMaxTouchRows) {
      s_row_top[s_row_count] = top;
      s_row_bottom[s_row_count] = y;
      ++s_row_count;
    }
#endif
  }
  y += kSectionGap;
  // Color key: each word drawn in its radar icon color (dark-radar palette).
  (void)legend;
  const char* words[] = {"Military", "Helicopter", "Private"};
  const uint16_t colors[] = {tft.color565(255, 50, 40), tft.color565(64, 170, 255),
                             tft.color565(235, 235, 235)};
  displayFontApply(tft, displayFontDetail());
  const int gap = ui::sc(14);
  int total = 2 * gap;
  for (const char* w : words) {
    total += tft.textWidth(w);
  }
  int x = kCenterX - total / 2;
  tft.setTextDatum(TextDatum::TopLeft);
  for (int i = 0; i < 3; ++i) {
    tft.setTextColor(colors[i], bg);
    tft.drawString(words[i], x, y);
    x += tft.textWidth(words[i]) + gap;
  }
  tft.setTextDatum(TextDatum::TopCenter);
}

void toggleAlertsRow(AlertsAdjustRow row) {
  switch (row) {
    case AlertsAdjustRow::TypeColors:
      services::alert::toggleTypeColors();
      break;
    case AlertsAdjustRow::Military:
      services::alert::toggleMilitaryAlert();
      break;
    case AlertsAdjustRow::Emergency:
      services::alert::toggleEmergencyAlert();
      break;
    case AlertsAdjustRow::Helicopters:
      services::alert::toggleHelicopterAlert();
      break;
    case AlertsAdjustRow::Private:
      services::alert::togglePrivateAlert();
      break;
    case AlertsAdjustRow::When:
      services::alert::toggleAlertOnApproach();
      break;
    case AlertsAdjustRow::OverheadAny:
      services::alert::toggleOverheadAny();
      break;
    default:
      break;
  }
}

}  // namespace

namespace ui {

void infoScreenResetToMain() { infoScreenSetPage(ui::InfoSettingsPage::Main); }

int8_t infoScreenTouch(int16_t x, int16_t y) {
#if !FLIGHTSCNR_HAS_ENCODER
  if (s_page != InfoSettingsPage::Display && s_page != InfoSettingsPage::Colors &&
      s_page != InfoSettingsPage::Alerts) {
    return 0;
  }
  // Rows are drawn edge to edge; give each the gap below it so taps never fall through.
  int hit = -1;
  for (int i = 0; i < s_row_count; ++i) {
    const int bottom = (i + 1 < s_row_count) ? s_row_top[i + 1] : s_row_bottom[i] + ui::sc(6);
    const int top = (i == 0) ? s_row_top[i] - ui::sc(6) : s_row_top[i];
    if (y >= top && y < bottom) {
      hit = i;
      break;
    }
  }
  if (hit < 0) {
    return 0;
  }
  const int focused = (s_page == InfoSettingsPage::Display)  ? static_cast<int>(s_display_focus)
                      : (s_page == InfoSettingsPage::Colors) ? static_cast<int>(s_colors_focus)
                                                             : static_cast<int>(s_alerts_focus);
  if (hit != focused) {
    if (s_page == InfoSettingsPage::Display) {
      s_display_focus = static_cast<DisplayAdjustRow>(hit);
    } else if (s_page == InfoSettingsPage::Colors) {
      s_colors_focus = static_cast<ColorsAdjustRow>(hit);
    } else {
      s_alerts_focus = static_cast<AlertsAdjustRow>(hit);
    }
    return 0;
  }
  return x < kCenterX ? -1 : 1;
#else
  (void)x;
  (void)y;
  return 0;
#endif
}

void infoScreenResetDisplayFocus() {
  s_display_focus = DisplayAdjustRow::Brightness;
}

void infoScreenCycleDisplayFocus() {
  switch (s_display_focus) {
    case DisplayAdjustRow::Brightness:
      s_display_focus = DisplayAdjustRow::Units;
      break;
    case DisplayAdjustRow::Units:
      s_display_focus = DisplayAdjustRow::Range;
      break;
    case DisplayAdjustRow::Range:
      s_display_focus = DisplayAdjustRow::Compass;
      break;
    case DisplayAdjustRow::Compass:
      s_display_focus = DisplayAdjustRow::Rings;
      break;
    case DisplayAdjustRow::Rings:
      s_display_focus = DisplayAdjustRow::Facing;
      break;
    default:
      s_display_focus = DisplayAdjustRow::Brightness;
      break;
  }
}

bool infoScreenFacingFocused() {
  return s_page == ui::InfoSettingsPage::Display &&
         s_display_focus == DisplayAdjustRow::Facing;
}

void infoScreenResetColorsFocus() { s_colors_focus = ColorsAdjustRow::Sweep; }

void infoScreenResetAlertsFocus() { s_alerts_focus = AlertsAdjustRow::TypeColors; }

void infoScreenCycleAlertsFocus() {
  const int next =
      (static_cast<int>(s_alerts_focus) + 1) % static_cast<int>(AlertsAdjustRow::Count);
  s_alerts_focus = static_cast<AlertsAdjustRow>(next);
}

void infoScreenCycleColorsFocus() {
  switch (s_colors_focus) {
    case ColorsAdjustRow::Sweep:
      s_colors_focus = ColorsAdjustRow::BlipDetails;
      break;
    case ColorsAdjustRow::BlipDetails:
      s_colors_focus = ColorsAdjustRow::DetailTimeout;
      break;
    case ColorsAdjustRow::DetailTimeout:
      s_colors_focus = ColorsAdjustRow::ClockTimeout;
      break;
    case ColorsAdjustRow::ClockTimeout:
      s_colors_focus = ColorsAdjustRow::IdleClock;
      break;
    case ColorsAdjustRow::IdleClock:
      s_colors_focus = ColorsAdjustRow::Color;
      break;
    case ColorsAdjustRow::Color:
      s_colors_focus = ColorsAdjustRow::BeepOn;
      break;
    case ColorsAdjustRow::BeepOn:
#if FLIGHTSCNR_HAS_HAPTIC
      s_colors_focus = ColorsAdjustRow::Sweep;
#else
      s_colors_focus = ColorsAdjustRow::BeepTone;
#endif
      break;
    default:
      s_colors_focus = ColorsAdjustRow::Sweep;
      break;
  }
}

InfoSettingsPage infoScreenPage() { return s_page; }

void infoScreenSetPage(InfoSettingsPage page) {
  s_page = page;
#if !FLIGHTSCNR_HAS_KNOB_BUTTON
  s_wifi_reset_armed_ms = 0;
#endif
}

MainPageTap infoScreenMainTap(int16_t x, int16_t y) {
  (void)x;
#if !FLIGHTSCNR_HAS_KNOB_BUTTON
  if (s_page != InfoSettingsPage::Main || s_wifi_reset_row_top < 0) {
    return MainPageTap::None;
  }
  // Generous hit box: the row plus a little margin above and below.
  if (y < s_wifi_reset_row_top - ui::sc(10) || y >= s_wifi_reset_row_bottom + ui::sc(10)) {
    // A tap anywhere else cancels a pending confirm.
    if (s_wifi_reset_armed_ms != 0) {
      s_wifi_reset_armed_ms = 0;
      return MainPageTap::Cancelled;
    }
    return MainPageTap::None;
  }
  const unsigned long now = millis();
  if (s_wifi_reset_armed_ms != 0 && now - s_wifi_reset_armed_ms < kWifiResetConfirmMs) {
    s_wifi_reset_armed_ms = 0;
    return MainPageTap::ResetConfirmed;
  }
  s_wifi_reset_armed_ms = now != 0 ? now : 1;
  return MainPageTap::ResetArmed;
#else
  (void)y;
  return MainPageTap::None;
#endif
}

bool infoScreenWifiResetTimedOut() {
#if !FLIGHTSCNR_HAS_KNOB_BUTTON
  if (s_wifi_reset_armed_ms != 0 && millis() - s_wifi_reset_armed_ms >= kWifiResetConfirmMs) {
    s_wifi_reset_armed_ms = 0;
    return true;
  }
#endif
  return false;
}

void infoScreenDraw() {
  tft.beginOffscreen();
  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t label_fg = tft.color565(180, 200, 220);
  const uint16_t hint_fg = tft.color565(120, 140, 160);

  tft.fillScreen(bg);

  if (s_page == ui::InfoSettingsPage::Main) {
    drawMainPage(bg, fg, label_fg, hint_fg);
  } else if (s_page == ui::InfoSettingsPage::Display) {
    drawDisplayPage(bg, fg, label_fg);
  } else if (s_page == ui::InfoSettingsPage::Colors) {
    drawColorsPage(bg, fg, label_fg);
  } else {
    drawAlertsPage(bg, fg, label_fg);
  }

  tft.setTextDatum(TextDatum::TopLeft);
  tft.endOffscreen();
}

void infoScreenHandleKnob(int8_t delta) {
  if (delta == 0) {
    return;
  }

  if (s_page == ui::InfoSettingsPage::Alerts) {
    // Every row is on/off: any dial step or side tap toggles it.
    toggleAlertsRow(s_alerts_focus);
    infoScreenDraw();
    return;
  }

  if (s_page == ui::InfoSettingsPage::Colors) {
    switch (s_colors_focus) {
      case ColorsAdjustRow::Sweep:
        ui::displayPrefsToggleSweepLine();
        break;
      case ColorsAdjustRow::BlipDetails:
        ui::displayPrefsToggleHideBlipDetails();
        break;
      case ColorsAdjustRow::DetailTimeout:
        ui::displayPrefsFlightDetailTimeoutStep(delta);
        break;
      case ColorsAdjustRow::ClockTimeout:
        ui::displayPrefsClockWeatherTimeoutStep(delta);
        break;
      case ColorsAdjustRow::IdleClock:
        ui::displayPrefsToggleAutoIdleClock();
        break;
      case ColorsAdjustRow::Color:
        ui::radar::accentStep(delta);
        break;
      case ColorsAdjustRow::BeepOn:
        hardware::buzzerSetEnabled(!hardware::buzzerEnabled());
        break;
#if !FLIGHTSCNR_HAS_HAPTIC
      case ColorsAdjustRow::BeepTone:
        hardware::buzzerToneStep(delta);
        break;
#endif
    }
    infoScreenDraw();
    return;
  }

  if (s_page != ui::InfoSettingsPage::Display) {
    return;
  }

  switch (s_display_focus) {
    case DisplayAdjustRow::Brightness:
      hardware::displayBrightnessStep(delta);
      break;
    case DisplayAdjustRow::Units:
      ui::radar::cycleDistanceUnits();
      break;
    case DisplayAdjustRow::Range:
      ui::radar::scaleStep(delta);
      services::alert::notifyRangeChanged();
      break;
    case DisplayAdjustRow::Compass:
      ui::radar::toggleCompassRose();
      break;
    case DisplayAdjustRow::Rings:
      ui::radar::gridModeStep(delta);
      break;
    case DisplayAdjustRow::Facing:
      // Dial enters orientation adjust from main.cpp; no-op here.
      return;
  }
  infoScreenDraw();
}

}  // namespace ui
