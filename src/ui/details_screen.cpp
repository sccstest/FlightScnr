#include "ui/details_screen.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <WiFi.h>

#include "config.h"
#include "hardware/display.h"
#include "hardware/display_font.h"
#include "services/adsb_client.h"
#include "services/device_identity.h"
#include "services/ota_github.h"
#include "ui/radar_display.h"
#include "ui/radar_theme.h"
#include "ui/ui_scale.h"

namespace ui {
namespace {

constexpr int kBezelInsetPx = ui::sc(10);
constexpr int kTextPadPx = ui::sc(6);
constexpr int kLineGap = ui::sc(4);
/** Space between the About sections (version, network, credits). */
constexpr int kSectionGap = ui::sc(12);

const int kCenterX = config::kDisplayWidth / 2;
const int kCenterY = config::kDisplayHeight / 2;
const int kCircleRadius = kCenterX - kBezelInsetPx;

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
  // Work from a copy: snprintf must not read the buffer it writes. ASCII "...":
  // the device fonts only have 0x20-0x7E.
  char original[64];
  strncpy(original, text, sizeof(original) - 1);
  original[sizeof(original) - 1] = '\0';
  const size_t raw_len = strlen(original);
  for (size_t n = raw_len; n > 0; --n) {
    snprintf(text, len, "%.*s...", static_cast<int>(n), original);
    if (tft.textWidth(text) <= max_width_px) {
      return;
    }
  }
  strncpy(text, "...", len);
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

}  // namespace

void detailsScreenDraw(bool boot_splash) {
  tft.beginOffscreen();
  const uint16_t bg = tft.color565(0, 0, 0);
  const uint16_t fg = tft.color565(255, 255, 255);
  const uint16_t label_fg = tft.color565(180, 200, 220);
  const uint16_t hint_fg = tft.color565(120, 140, 160);

  char version_line[40];
  snprintf(version_line, sizeof(version_line), "Version: %s", config::kFirmwareVersion);

  const bool show_update = services::ota_github::updateAvailable();

  // Network status: the .local name (or the IP where .local names don't resolve)
  // is how the owner reaches the settings page; the feed line explains an empty radar.
  const bool wifi_up = WiFi.status() == WL_CONNECTED;
  char host_line[48];
  snprintf(host_line, sizeof(host_line), "%s.local",
           services::device_identity::portalHostname());
  char ip_line[40];
  char feed_line[40];
  uint16_t feed_fg = label_fg;
  if (wifi_up) {
    snprintf(ip_line, sizeof(ip_line), "or http://%s", WiFi.localIP().toString().c_str());
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
    snprintf(ip_line, sizeof(ip_line), "Wi-Fi: not connected");
    snprintf(feed_line, sizeof(feed_line), "Retrying saved networks");
    feed_fg = hint_fg;
  }

  InfoLine lines[16];
  int gaps_before[16] = {};
  size_t n = 0;
  auto add = [&](const char* text, UiTextStyle style, uint16_t color, int gap_before) {
    if (n < sizeof(lines) / sizeof(lines[0])) {
      lines[n] = {text, style, color};
      gaps_before[n] = gap_before;
      ++n;
    }
  };

  add("About", displayFontTitle(), fg, 0);
  add(version_line, displayFontBody(), fg, 0);
  if (show_update) {
    add("Update available", displayFontDetail(), tft.color565(255, 200, 80), 0);
    add("Install it in Phone settings", displayFontDetail(), hint_fg, 0);
  }
  if (wifi_up) {
    add("Phone settings:", displayFontDetail(), label_fg, kSectionGap);
    add(host_line, displayFontBody(), fg, 0);
    add(ip_line, displayFontDetail(), label_fg, 0);
  } else {
    add(ip_line, displayFontDetail(), tft.color565(255, 90, 80), kSectionGap);
  }
  add(feed_line, displayFontDetail(), feed_fg, 0);
  // Credits (CC BY-NC-SA 4.0: keep FlightScnr and the case designer named).
  add("FlightScnr by", displayFontDetail(), label_fg, kSectionGap);
  add("Yash Mulgaonkar", displayFontDetail(), label_fg, 0);
  add("Case design: nin3D", displayFontDetail(), label_fg, 0);
  if (!boot_splash) {
    add("Swipe down to close", displayFontScale(), hint_fg, kSectionGap);
  }

  int block_h = 0;
  for (size_t i = 0; i < n; ++i) {
    block_h += gaps_before[i] + displayFontHeight(tft, lines[i].style);
    if (i + 1 < n) {
      block_h += kLineGap;
    }
  }

  tft.fillScreen(bg);

  int y = kCenterY - block_h / 2;
  if (y < kBezelInsetPx) {
    y = kBezelInsetPx;
  }
  for (size_t i = 0; i < n; ++i) {
    y += gaps_before[i];
    drawCenterLine(lines[i].text, &y, lines[i].style, lines[i].color, bg);
  }

  tft.setTextDatum(TextDatum::TopLeft);
  tft.endOffscreen();
}

}  // namespace ui
