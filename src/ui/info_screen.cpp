#include "ui/info_screen.h"

#include <WiFi.h>

#include <cctype>
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
#include "services/rain_radar.h"
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
#include "services/sky_events.h"
#endif
#include "services/wifi_setup.h"
#include "ui/display_prefs.h"
#include "ui/qr_draw.h"
#include "ui/radar_accent.h"
#include "ui/radar_display.h"
#include "ui/radar_scale.h"
#include "ui/radar_theme.h"
#include "ui/settings_rows.h"
#include "ui/ui_scale.h"

namespace {

constexpr int kBezelInsetPx = ui::sc(10);
constexpr int kTitleGap = ui::sc(6);
constexpr int kLineGap = ui::sc(4);
constexpr int kSectionGap = ui::sc(6);

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;

// ---------------------------------------------------------------------------
// Rows and pages
// ---------------------------------------------------------------------------

/** Every adjustable setting. Pages below list which rows they show, in order. */
enum class Row : uint8_t {
  Brightness,
  RadarColor,
  Units,
  Range,
  Rings,
  Compass,
  Facing,
  Sweep,
  PlaneLabels,
  TypeColors,
  IdleClock,
  DetailTimeout,
  ClockTimeout,
  BeepOn,
#if !FLIGHTSCNR_HAS_HAPTIC
  BeepTone,
#endif
  Military,
  Emergency,
  Helicopters,
  Private,
  When,
  OverheadAny,
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  Airliners,
  OverheadSign,
  Rain,
  Notable,
  SkyLaunches,
  SkyIss,
#endif
};

struct PageDef {
  ui::InfoSettingsPage page;
  const char* title;
  const Row* rows;
  uint8_t row_count;
  bool type_key;  ///< draw the "Military Heli Private" color key under the rows
};

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
// 4C: at most seven ~70 px rows per page so each is easy to tap at desk distance.
// The longest labels sit mid-page where the round face is widest.
constexpr Row kDisplayRows[] = {Row::Brightness, Row::RadarColor, Row::Units, Row::Range,
                                Row::Rings,      Row::Compass,    Row::Facing};
constexpr Row kRadarRows[] = {Row::Sweep,     Row::PlaneLabels,  Row::TypeColors,
                              Row::Airliners, Row::OverheadSign, Row::Rain};
// Long labels first: the seventh row sits low on the round face where it is narrow.
constexpr Row kAlertRows[] = {Row::OverheadAny, Row::When,        Row::Notable,  Row::Private,
                              Row::Helicopters, Row::Emergency,   Row::Military};
constexpr Row kSoundRows[] = {Row::BeepOn, Row::BeepTone, Row::SkyLaunches, Row::SkyIss};
constexpr Row kTimerRows[] = {Row::DetailTimeout, Row::ClockTimeout, Row::IdleClock};

#define FS_ROWS(a) a, static_cast<uint8_t>(sizeof(a) / sizeof(a[0]))
const PageDef kPages[] = {
    {ui::InfoSettingsPage::Main, "Status", nullptr, 0, false},
    {ui::InfoSettingsPage::Display, "Display", FS_ROWS(kDisplayRows), false},
    {ui::InfoSettingsPage::Colors, "Radar", FS_ROWS(kRadarRows), true},
    {ui::InfoSettingsPage::Alerts, "Alerts", FS_ROWS(kAlertRows), false},
    {ui::InfoSettingsPage::Sound, "Sound & sky", FS_ROWS(kSoundRows), false},
    {ui::InfoSettingsPage::Timers, "Screen timers", FS_ROWS(kTimerRows), false},
};
#else
// Knob boards: the original three option pages (compact rows, dial to adjust).
constexpr Row kDisplayRows[] = {Row::Brightness, Row::Units, Row::Range,
                                Row::Compass,    Row::Rings, Row::Facing};
constexpr Row kColorsRows[] = {Row::Sweep,        Row::PlaneLabels, Row::DetailTimeout,
                               Row::ClockTimeout, Row::IdleClock,   Row::RadarColor,
                               Row::BeepOn,
#if !FLIGHTSCNR_HAS_HAPTIC
                               Row::BeepTone,
#endif
};
constexpr Row kAlertRows[] = {Row::TypeColors, Row::Military, Row::Emergency, Row::Helicopters,
                              Row::Private,    Row::When,     Row::OverheadAny};

#define FS_ROWS(a) a, static_cast<uint8_t>(sizeof(a) / sizeof(a[0]))
const PageDef kPages[] = {
    {ui::InfoSettingsPage::Main, "Status", nullptr, 0, false},
    {ui::InfoSettingsPage::Display, "Display", FS_ROWS(kDisplayRows), false},
    {ui::InfoSettingsPage::Colors, "Radar & sound", FS_ROWS(kColorsRows), false},
    {ui::InfoSettingsPage::Alerts, "Alerts", FS_ROWS(kAlertRows), true},
};
#endif
#undef FS_ROWS

constexpr int kPageCount = static_cast<int>(sizeof(kPages) / sizeof(kPages[0]));

int s_page_index = 0;
uint8_t s_focus = 0;

const PageDef& currentPage() { return kPages[s_page_index]; }

int pageIndexFor(ui::InfoSettingsPage page) {
  for (int i = 0; i < kPageCount; ++i) {
    if (kPages[i].page == page) {
      return i;
    }
  }
  return 0;
}

bool focusedRow(Row* out) {
  const PageDef& p = currentPage();
  if (p.row_count == 0 || s_focus >= p.row_count) {
    return false;
  }
  *out = p.rows[s_focus];
  return true;
}

// Row bands from the last draw, for touch hit tests.
int s_rows_top = 0;
int s_row_pitch = 0;
int s_rows_drawn = 0;

#if !FLIGHTSCNR_HAS_KNOB_BUTTON
// No knob button to hold for a Wi-Fi wipe: the status page gets a "Reset Wi-Fi"
// button instead. First tap arms it ("Tap again to reset"), a second tap within
// kWifiResetConfirmMs wipes saved networks and reboots into the setup portal.
constexpr unsigned long kWifiResetConfirmMs = 5000UL;
unsigned long s_wifi_reset_armed_ms = 0;
int s_wifi_reset_row_top = -1;
int s_wifi_reset_row_bottom = -1;
#endif

// ---------------------------------------------------------------------------
// Text helpers
// ---------------------------------------------------------------------------

void drawCenterLine(const char* text, int* y, UiTextStyle style, uint16_t fg, uint16_t bg) {
  displayFontApply(tft, style);
  const int h = displayFontHeight(tft, style);
  char line[64];
  strncpy(line, text, sizeof(line) - 1);
  line[sizeof(line) - 1] = '\0';
  ui::settings_rows::fitText(line, sizeof(line), style,
                             2 * ui::settings_rows::halfWidthForBand(*y, h));
  displayFontApply(tft, style);
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString(line, kCenterX, *y);
  *y += h + kLineGap;
}

/** "8mi" -> "8 mi": units always get a space. */
void spaceBeforeUnit(const char* in, char* out, size_t len) {
  size_t o = 0;
  for (size_t i = 0; in[i] != '\0' && o + 1 < len; ++i) {
    if (i > 0 && isdigit(static_cast<unsigned char>(in[i - 1])) &&
        isalpha(static_cast<unsigned char>(in[i])) && o + 2 < len) {
      out[o++] = ' ';
    }
    out[o++] = in[i];
  }
  out[o] = '\0';
}

const char* onOff(bool v) { return v ? "on" : "off"; }

/** Timeout labels: "Manual" means the screen stays until you close it. */
const char* timeoutText(const char* label) {
  return strcmp(label, "Manual") == 0 ? "stays open" : label;
}

void formatRow(Row row, char* out, size_t len) {
  switch (row) {
    case Row::Brightness:
      snprintf(out, len, "Brightness: %u%%",
               static_cast<unsigned>(hardware::displayBrightnessPercent()));
      break;
    case Row::RadarColor:
      snprintf(out, len, "Radar color: %s", ui::radar::accentColorName());
      break;
    case Row::Units:
      snprintf(out, len, "Units: %s", ui::radar::distanceUnitLabel());
      break;
    case Row::Range: {
      char tag[12];
      char spaced[16];
      ui::radar::formatActiveScaleTag(tag, sizeof(tag));
      spaceBeforeUnit(tag, spaced, sizeof(spaced));
      snprintf(out, len, "Range: %s", spaced);
      break;
    }
    case Row::Rings:
      snprintf(out, len, "Range rings: %s", ui::radar::gridModeLabel());
      break;
    case Row::Compass:
      snprintf(out, len, "Compass rose: %s", onOff(ui::radar::showCompassRose()));
      break;
    case Row::Facing: {
      char tag[12];
      ui::radar::facingLabel(tag, sizeof(tag));
      snprintf(out, len, "Facing: %s", tag);
      break;
    }
    case Row::Sweep:
      snprintf(out, len, "Radar sweep: %s", onOff(ui::displayPrefsSweepLineEnabled()));
      break;
    case Row::PlaneLabels:
      // Same setting as "Hide Blip Details", worded the positive way round.
      snprintf(out, len, "Plane labels: %s", onOff(!ui::displayPrefsHideBlipDetails()));
      break;
    case Row::TypeColors:
      snprintf(out, len, "Color planes by type: %s",
               onOff(services::alert::typeColorsEnabled()));
      break;
    case Row::IdleClock:
      snprintf(out, len, "Idle clock: %s", onOff(ui::displayPrefsAutoIdleClockEnabled()));
      break;
    case Row::DetailTimeout:
      snprintf(out, len, "Flight detail: %s",
               timeoutText(ui::displayPrefsFlightDetailTimeoutLabel()));
      break;
    case Row::ClockTimeout:
      snprintf(out, len, "Clock/forecast: %s",
               timeoutText(ui::displayPrefsClockWeatherTimeoutLabel()));
      break;
    case Row::BeepOn:
#if FLIGHTSCNR_HAS_HAPTIC
      snprintf(out, len, "Vibration: %s", onOff(hardware::buzzerEnabled()));
#elif FLIGHTSCNR_HAS_SPEAKER
      snprintf(out, len, "Alert sound: %s", onOff(hardware::buzzerEnabled()));
#else
      snprintf(out, len, "UI beep: %s", onOff(hardware::buzzerEnabled()));
#endif
      break;
#if !FLIGHTSCNR_HAS_HAPTIC
    case Row::BeepTone:
#if FLIGHTSCNR_HAS_SPEAKER
      // Letters A-E read as a code; show the step out of five.
      snprintf(out, len, "Chime volume: %d of 5", hardware::buzzerToneLetter() - 'A' + 1);
#else
      snprintf(out, len, "Beep tone: %c", hardware::buzzerToneLetter());
#endif
      break;
#endif
    case Row::Military:
      snprintf(out, len, "Military: %s", onOff(services::alert::militaryAlertEnabled()));
      break;
    case Row::Emergency:
      snprintf(out, len, "Emergency: %s", onOff(services::alert::emergencyAlertEnabled()));
      break;
    case Row::Helicopters:
      snprintf(out, len, "Helicopters: %s", onOff(services::alert::helicopterAlertEnabled()));
      break;
    case Row::Private:
      snprintf(out, len, "Private planes: %s", onOff(services::alert::privateAlertEnabled()));
      break;
    case Row::When:
      snprintf(out, len, "Alert when: %s",
               services::alert::alertOnApproachEnabled() ? "overhead" : "entering");
      break;
    case Row::OverheadAny:
      snprintf(out, len, "Overhead alert: %s", onOff(services::alert::overheadAnyEnabled()));
      break;
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    case Row::Airliners:
      snprintf(out, len, "Airliners: %s",
               services::alert::airlinersHidden() ? "hidden" : "shown");
      break;
    case Row::OverheadSign:
      snprintf(out, len, "Overhead sign: %s", onOff(ui::displayPrefsOverheadSignEnabled()));
      break;
    case Row::Rain:
      snprintf(out, len, "Rain: %s", onOff(services::rain::enabled()));
      break;
    case Row::Notable:
      snprintf(out, len, "Special planes alert: %s",
               onOff(services::alert::notableAlertEnabled()));
      break;
    case Row::SkyLaunches:
      snprintf(out, len, "Rocket launches: %s", onOff(services::sky::launchesEnabled()));
      break;
    case Row::SkyIss:
      snprintf(out, len, "ISS passes: %s", onOff(services::sky::issEnabled()));
      break;
#endif
  }
}

/**
 * Step the row's value. Ordered values (brightness, range, volume, timeouts) stop
 * at the ends instead of wrapping around; on/off rows toggle.
 */
void stepRow(Row row, int8_t delta) {
  const int dir = delta > 0 ? 1 : -1;
  switch (row) {
    case Row::Brightness:
      hardware::displayBrightnessStep(static_cast<int8_t>(dir));
      break;
    case Row::RadarColor:
      ui::radar::accentStep(delta);
      break;
    case Row::Units:
      ui::radar::cycleDistanceUnits();
      break;
    case Row::Range: {
      const int idx = ui::radar::scaleActiveIndex();
      const int last = static_cast<int>(ui::radar::kRangeMileOptionCount) - 1;
      if ((dir < 0 && idx <= 0) || (dir > 0 && idx >= last)) {
        break;
      }
      ui::radar::scaleStep(static_cast<int8_t>(dir));
      services::alert::notifyRangeChanged();
      break;
    }
    case Row::Rings:
      ui::radar::gridModeStep(delta);
      break;
    case Row::Compass:
      ui::radar::toggleCompassRose();
      break;
    case Row::Facing:
      // The dial / side tap opens orientation adjust from main.cpp.
      break;
    case Row::Sweep:
      ui::displayPrefsToggleSweepLine();
      break;
    case Row::PlaneLabels:
      ui::displayPrefsToggleHideBlipDetails();
      break;
    case Row::TypeColors:
      services::alert::toggleTypeColors();
      break;
    case Row::IdleClock:
      ui::displayPrefsToggleAutoIdleClock();
      break;
    case Row::DetailTimeout: {
      // Shown in the order 10, 20, 30 sec, stays open (the stored list starts
      // with "stays open", so its ends are 0 s and 10 s).
      const unsigned long sec = ui::displayPrefsFlightDetailTimeoutMs() / 1000UL;
      if ((dir < 0 && sec == 10) || (dir > 0 && sec == 0)) {
        break;
      }
      ui::displayPrefsFlightDetailTimeoutStep(static_cast<int8_t>(dir));
      break;
    }
    case Row::ClockTimeout: {
      // Shown in the order 5, 10, 15 sec, stays open.
      const unsigned long sec = ui::displayPrefsClockWeatherTimeoutMs() / 1000UL;
      if ((dir < 0 && sec == 5) || (dir > 0 && sec == 0)) {
        break;
      }
      ui::displayPrefsClockWeatherTimeoutStep(static_cast<int8_t>(dir));
      break;
    }
    case Row::BeepOn:
      hardware::buzzerSetEnabled(!hardware::buzzerEnabled());
      break;
#if !FLIGHTSCNR_HAS_HAPTIC
    case Row::BeepTone: {
      const char letter = hardware::buzzerToneLetter();
      if ((dir < 0 && letter <= 'A') || (dir > 0 && letter >= 'E')) {
        break;
      }
      hardware::buzzerToneStep(static_cast<int8_t>(dir));
      break;
    }
#endif
    case Row::Military:
      services::alert::toggleMilitaryAlert();
      break;
    case Row::Emergency:
      services::alert::toggleEmergencyAlert();
      break;
    case Row::Helicopters:
      services::alert::toggleHelicopterAlert();
      break;
    case Row::Private:
      services::alert::togglePrivateAlert();
      break;
    case Row::When:
      services::alert::toggleAlertOnApproach();
      break;
    case Row::OverheadAny:
      services::alert::toggleOverheadAny();
      break;
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    case Row::Airliners:
      services::alert::toggleHideAirliners();
      break;
    case Row::OverheadSign:
      ui::displayPrefsToggleOverheadSign();
      break;
    case Row::Rain:
      services::rain::toggleEnabled();
      break;
    case Row::Notable:
      services::alert::toggleNotableAlert();
      break;
    case Row::SkyLaunches:
      services::sky::toggleLaunches();
      break;
    case Row::SkyIss:
      services::sky::toggleIss();
      break;
#endif
  }
}

// ---------------------------------------------------------------------------
// Status page (page 1)
// ---------------------------------------------------------------------------

/**
 * WiFi.SSID() is an RPC to the C6 on the 4C: read it at most every 30 s, not on
 * every redraw (the page redraws on each tap and on the Wi-Fi reset timer).
 */
constexpr unsigned long kSsidCacheMs = 30000UL;
char s_ssid_cache[33] = {};
unsigned long s_ssid_cache_ms = 0;
bool s_ssid_cache_valid = false;

const char* cachedSsid() {
  const unsigned long now = millis();
  if (!s_ssid_cache_valid || now - s_ssid_cache_ms >= kSsidCacheMs) {
    strncpy(s_ssid_cache, WiFi.SSID().c_str(), sizeof(s_ssid_cache) - 1);
    s_ssid_cache[sizeof(s_ssid_cache) - 1] = '\0';
    s_ssid_cache_ms = now;
    s_ssid_cache_valid = true;
  }
  return s_ssid_cache;
}

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
/** QR for the settings page URL (IP form: works even where .local names don't). */
constexpr int kStatusQrModulePx = 5;
#endif

void drawResetButton(int top, uint16_t fg, uint16_t bg) {
#if !FLIGHTSCNR_HAS_KNOB_BUTTON
  const bool armed = s_wifi_reset_armed_ms != 0;
  const char* label = armed ? "Tap again to reset" : "Reset Wi-Fi";
  const uint16_t color = armed ? tft.color565(255, 90, 80) : fg;
  const UiTextStyle style = displayFontDetail();
  displayFontApply(tft, style);
  const int text_w = tft.textWidth(label);
  const int h = ui::sc(34);
  int w = text_w + 2 * ui::sc(22);
  const int max_w = 2 * ui::settings_rows::halfWidthForBand(top, h);
  if (w > max_w) {
    w = max_w;
  }
  const int x = kCenterX - w / 2;
  const int r = h / 2;
  // Outlined pill: 2 px ring drawn as an outer fill with an inner bg fill.
  tft.fillRect(static_cast<int16_t>(x + r), static_cast<int16_t>(top),
               static_cast<int16_t>(w - 2 * r), static_cast<int16_t>(h), color);
  tft.fillCircle(static_cast<int16_t>(x + r), static_cast<int16_t>(top + r),
                 static_cast<int16_t>(r), color);
  tft.fillCircle(static_cast<int16_t>(x + w - r), static_cast<int16_t>(top + r),
                 static_cast<int16_t>(r), color);
  tft.fillRect(static_cast<int16_t>(x + r), static_cast<int16_t>(top + 2),
               static_cast<int16_t>(w - 2 * r), static_cast<int16_t>(h - 4), bg);
  tft.fillCircle(static_cast<int16_t>(x + r), static_cast<int16_t>(top + r),
                 static_cast<int16_t>(r - 2), bg);
  tft.fillCircle(static_cast<int16_t>(x + w - r), static_cast<int16_t>(top + r),
                 static_cast<int16_t>(r - 2), bg);
  tft.setTextDatum(TextDatum::MiddleCenter);
  tft.setTextColor(color, bg);
  tft.drawString(label, kCenterX, top + h / 2);
  tft.setTextDatum(TextDatum::TopCenter);
  s_wifi_reset_row_top = top;
  s_wifi_reset_row_bottom = top + h;
#else
  (void)top;
  (void)fg;
  (void)bg;
#endif
}

void drawStatusPage(const char* title, uint16_t bg, uint16_t fg, uint16_t label_fg,
                    uint16_t hint_fg) {
  const bool wifi_up = WiFi.status() == WL_CONNECTED;
  if (!wifi_up) {
    s_ssid_cache_valid = false;  // re-read the name after a reconnect
  }

  char host_line[48];
  snprintf(host_line, sizeof(host_line), "%s.local",
           services::device_identity::portalHostname());
  char wifi_line[48];
  char feed_line[40];
  uint16_t wifi_fg = label_fg;
  uint16_t feed_fg = label_fg;
  if (wifi_up) {
    snprintf(wifi_line, sizeof(wifi_line), "Wi-Fi: %s", cachedSsid());
    if (services::adsb::feedLooksDown() ||
        ui::radarDisplayLinkStatus() == ui::RadarLinkStatus::FeedDown) {
      snprintf(feed_line, sizeof(feed_line), "Flight data: offline");
      feed_fg = tft.color565(255, 170, 20);
    } else if (services::adsb::lastFetchOkAgeMs() == UINT32_MAX) {
      snprintf(feed_line, sizeof(feed_line), "Flight data: starting");
    } else {
      snprintf(feed_line, sizeof(feed_line), "Flight data: OK");
    }
  } else {
    snprintf(wifi_line, sizeof(wifi_line), "Wi-Fi: not connected");
    wifi_fg = tft.color565(255, 90, 80);
    snprintf(feed_line, sizeof(feed_line), "Retrying saved networks");
    feed_fg = hint_fg;
  }
  const bool show_routes_credit = services::apikeys::useAdsbDb();

  const UiTextStyle title_style = displayFontTitle();
  const UiTextStyle label_style = displayFontDetail();
  const UiTextStyle host_style = displayFontBody();
  const UiTextStyle small_style = displayFontScale();
  const int title_h = displayFontHeight(tft, title_style);
  const int label_h = displayFontHeight(tft, label_style);
  const int host_h = displayFontHeight(tft, host_style);
  const int small_h = displayFontHeight(tft, small_style);

  int qr_px = 0;
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  char url[40] = {};
  if (wifi_up) {
    snprintf(url, sizeof(url), "http://%s/", WiFi.localIP().toString().c_str());
    qr_px = 37 * kStatusQrModulePx;  // version 3 + 4-module quiet zone each side
  }
#endif

  int block_h = title_h + kTitleGap;
  if (wifi_up) {
    block_h += label_h + kLineGap + host_h + kSectionGap;
    if (qr_px > 0) {
      block_h += qr_px + kSectionGap;
    }
  }
  block_h += 2 * (label_h + kLineGap);
  if (show_routes_credit) {
    block_h += small_h + kLineGap;
  }
#if !FLIGHTSCNR_HAS_KNOB_BUTTON
  block_h += kSectionGap + ui::sc(34);
#endif

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  displayFontApply(tft, title_style);
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString(title, kCenterX, y);
  y += title_h + kTitleGap;

  if (wifi_up) {
    // The web address is how the owner changes settings: lead with it, in white.
    drawCenterLine("Phone settings:", &y, label_style, label_fg, bg);
    drawCenterLine(host_line, &y, host_style, fg, bg);
    y += kSectionGap - kLineGap;
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
    if (qr_px > 0) {
      if (!drawUrlQr(kCenterX, y + qr_px / 2, kStatusQrModulePx, url)) {
        drawCenterLine(url, &y, label_style, fg, bg);
      } else {
        y += qr_px;
      }
      y += kSectionGap;
    }
#endif
  }
  drawCenterLine(wifi_line, &y, label_style, wifi_fg, bg);
  drawCenterLine(feed_line, &y, label_style, feed_fg, bg);
  if (show_routes_credit) {
    // Route data attribution (adsbdb.com asks for it).
    drawCenterLine("Routes: adsbdb.com", &y, small_style, hint_fg, bg);
  }

#if !FLIGHTSCNR_HAS_KNOB_BUTTON
  y += kSectionGap - kLineGap;
  drawResetButton(y, label_fg, bg);
#endif
}

// ---------------------------------------------------------------------------
// Option pages
// ---------------------------------------------------------------------------

void drawTypeKey(int y, uint16_t bg) {
  // Color key: each word drawn in its radar icon color (dark-radar palette).
  const char* words[] = {"Military", "Heli", "Private"};
  const uint16_t colors[] = {tft.color565(255, 50, 40), tft.color565(64, 170, 255),
                             tft.color565(235, 235, 235)};
  const UiTextStyle style = displayFontDetail();
  displayFontApply(tft, style);
  const int gap = ui::sc(14);
  int total = 2 * gap;
  for (const char* w : words) {
    total += tft.textWidth(w);
  }
  if (total > 2 * ui::settings_rows::halfWidthForBand(y, displayFontHeight(tft, style))) {
    return;
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

void drawOptionPage(const PageDef& page, const char* title, uint16_t bg, uint16_t fg,
                    uint16_t hint_fg) {
  const ui::settings_rows::Colors colors = ui::settings_rows::defaultColors();
  const UiTextStyle row_style = displayFontBody();
  const int pitch = ui::settings_rows::rowPitch(row_style);
  const int title_h = displayFontHeight(tft, displayFontTitle());
  const int key_h = page.type_key ? kSectionGap + displayFontHeight(tft, displayFontDetail()) : 0;
  // The last page points at clock settings (one more swipe left).
  const bool show_more_hint = (s_page_index == kPageCount - 1);
  const int hint_h = show_more_hint ? kSectionGap + displayFontHeight(tft, displayFontScale()) : 0;
  const int block_h = title_h + kTitleGap + pitch * page.row_count + key_h + hint_h;

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }

  displayFontApply(tft, displayFontTitle());
  tft.setTextDatum(TextDatum::TopCenter);
  tft.setTextColor(fg, bg);
  tft.drawString(title, kCenterX, y);
  y += title_h + kTitleGap;

  s_rows_top = y;
  s_row_pitch = pitch;
  s_rows_drawn = page.row_count;
  for (uint8_t i = 0; i < page.row_count; ++i) {
    char text[48];
    formatRow(page.rows[i], text, sizeof(text));
    ui::settings_rows::drawOptionRow(text, y, pitch, row_style, i == s_focus, colors);
    y += pitch;
  }

  if (page.type_key) {
    y += kSectionGap;
    drawTypeKey(y, bg);
    y += displayFontHeight(tft, displayFontDetail());
  }
  if (show_more_hint) {
    y += kSectionGap;
    drawCenterLine("Swipe left: clock settings", &y, displayFontScale(), hint_fg, bg);
  }
}

/** Row index under (x, y) on the current option page, or -1. */
int rowHit(int16_t y) {
  if (s_rows_drawn <= 0 || s_row_pitch <= 0) {
    return -1;
  }
  // A little slack above the first and below the last row.
  const int slack = ui::sc(8);
  const int top = s_rows_top - slack;
  const int bottom = s_rows_top + s_rows_drawn * s_row_pitch + slack;
  if (y < top || y >= bottom) {
    return -1;
  }
  int hit = (y - s_rows_top) / s_row_pitch;
  if (y < s_rows_top) {
    hit = 0;
  }
  if (hit >= s_rows_drawn) {
    hit = s_rows_drawn - 1;
  }
  return hit;
}

void resetFocus() { s_focus = 0; }

void cycleFocus() {
  const PageDef& p = currentPage();
  if (p.row_count == 0) {
    s_focus = 0;
    return;
  }
  s_focus = static_cast<uint8_t>((s_focus + 1) % p.row_count);
}

}  // namespace

namespace ui {

void infoScreenResetToMain() { infoScreenSetPage(InfoSettingsPage::Main); }

InfoSettingsPage infoScreenPage() { return currentPage().page; }

void infoScreenSetPage(InfoSettingsPage page) {
  const int idx = pageIndexFor(page);
  if (idx != s_page_index) {
    s_focus = 0;
  }
  s_page_index = idx;
  s_rows_drawn = 0;
#if !FLIGHTSCNR_HAS_KNOB_BUTTON
  s_wifi_reset_armed_ms = 0;
  s_wifi_reset_row_top = -1;
#endif
}

bool infoScreenNextPage() {
  if (s_page_index + 1 >= kPageCount) {
    return false;
  }
  infoScreenSetPage(kPages[s_page_index + 1].page);
  return true;
}

bool infoScreenPrevPage() {
  if (s_page_index <= 0) {
    return false;
  }
  infoScreenSetPage(kPages[s_page_index - 1].page);
  return true;
}

bool infoScreenOnLastPage() { return s_page_index == kPageCount - 1; }

int infoScreenPageNumber() { return s_page_index + 1; }

int infoScreenPageCount() { return kPageCount; }

int8_t infoScreenTouch(int16_t x, int16_t y) {
#if !FLIGHTSCNR_HAS_ENCODER
  if (currentPage().row_count == 0) {
    return 0;
  }
  const int hit = rowHit(y);
  if (hit < 0) {
    return 0;
  }
  if (hit != s_focus) {
    s_focus = static_cast<uint8_t>(hit);
    return 0;
  }
  return x < kCenterX ? -1 : 1;
#else
  (void)x;
  (void)y;
  return 0;
#endif
}

void infoScreenResetDisplayFocus() { resetFocus(); }

void infoScreenCycleDisplayFocus() { cycleFocus(); }

bool infoScreenFacingFocused() {
  Row row;
  return focusedRow(&row) && row == Row::Facing;
}

void infoScreenResetColorsFocus() { resetFocus(); }

void infoScreenCycleColorsFocus() { cycleFocus(); }

void infoScreenResetAlertsFocus() { resetFocus(); }

void infoScreenCycleAlertsFocus() { cycleFocus(); }

MainPageTap infoScreenMainTap(int16_t x, int16_t y) {
#if !FLIGHTSCNR_HAS_KNOB_BUTTON
  if (currentPage().page != InfoSettingsPage::Main) {
    // Option pages without their own branch in main.cpp (the 4C's Sound and
    // Timers pages) arrive here: same handling as the other option pages.
    const int8_t step = infoScreenTouch(x, y);
    if (step != 0) {
      infoScreenHandleKnob(step);
    }
    return MainPageTap::Handled;
  }
  if (s_wifi_reset_row_top < 0) {
    return MainPageTap::None;
  }
  // Generous hit box: the button band plus a margin above and below.
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
  (void)x;
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

  const PageDef& page = currentPage();
  char title[32];
  snprintf(title, sizeof(title), "%s %d/%d", page.title, s_page_index + 1, kPageCount);
  if (page.row_count == 0) {
    drawStatusPage(title, bg, fg, label_fg, hint_fg);
  } else {
    drawOptionPage(page, title, bg, fg, hint_fg);
  }

  tft.setTextDatum(TextDatum::TopLeft);
  tft.endOffscreen();
}

void infoScreenHandleKnob(int8_t delta) {
  if (delta == 0) {
    return;
  }
  Row row;
  if (!focusedRow(&row) || row == Row::Facing) {
    return;
  }
  stepRow(row, delta);
  infoScreenDraw();
}

}  // namespace ui
