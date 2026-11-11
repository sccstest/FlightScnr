#include "ui/alert_flash.h"

#include <Arduino.h>
#include <cmath>

#include "config.h"
#include "ui/radar_display.h"
#include "ui/ui_scale.h"

namespace ui {
namespace {

constexpr unsigned long kAlertFlashMs = 10000;
/** Ring blink interval: 500ms on, 500ms off. */
constexpr unsigned long kRingBlinkPhaseMs = 500;
/** Outer bezel inset so the ring stays on the visible round panel. */
constexpr int kRingOuterInsetPx = ui::sc(2);
/** Radial thickness of the alert ring (px). */
constexpr int kRingThicknessPx = ui::sc(20);

constexpr int kRingOuterRadiusPx = config::kDisplayWidth / 2 - kRingOuterInsetPx;
constexpr int kRingInnerRadiusPx = kRingOuterRadiusPx - kRingThicknessPx;

unsigned long s_flash_end_ms = 0;
unsigned long s_flash_start_ms = 0;
bool s_active = false;
AlertFlashKind s_kind = AlertFlashKind::Tracked;

void fillRowSpan(uint16_t* row, int x0, int x1, int width, uint16_t color) {
  if (x0 < 0) {
    x0 = 0;
  }
  if (x1 >= width) {
    x1 = width - 1;
  }
  for (int x = x0; x <= x1; ++x) {
    row[x] = color;
  }
}

/** Solid annulus via horizontal spans into RAM — no SPI, no fillArc wedges. */
void paintRingBuffer(uint16_t* buf, int16_t w, int16_t h) {
  if (buf == nullptr || w <= 0 || h <= 0) {
    return;
  }

  const int cx = w / 2;
  const int cy = h / 2;
  const int ro = kRingOuterRadiusPx;
  const int ri = kRingInnerRadiusPx;
  const int ro2 = ro * ro;
  const int ri2 = ri * ri;
  // Same colors as the radar's type colors: red military, blue helicopter,
  // white private; green for other tracked (watch-list / emergency) aircraft.
  constexpr uint16_t kMilitaryRed = 0xD882;
  constexpr uint16_t kHelicopterBlue = 0x455F;  // ~(64,170,255)
  constexpr uint16_t kPrivateWhite = 0xE73C;    // ~(230,230,230)
  constexpr uint16_t kOverheadYellow = 0xFEA0;  // ~(255,212,0)
  constexpr uint16_t kTrackedGreen = 0x0666;
  constexpr uint16_t kNotableMagenta = 0xF81C;  // ~(255,0,230)
  constexpr uint16_t kNotableGold = 0xFDA0;     // ~(255,180,0)
  uint16_t ring_color = kTrackedGreen;
  switch (s_kind) {
    case AlertFlashKind::Military:
      ring_color = kMilitaryRed;
      break;
    case AlertFlashKind::Helicopter:
      ring_color = kHelicopterBlue;
      break;
    case AlertFlashKind::Private:
      ring_color = kPrivateWhite;
      break;
    case AlertFlashKind::Overhead:
      ring_color = kOverheadYellow;
      break;
    case AlertFlashKind::Notable:
      // Alternates every blink so it reads as "something special", not a class color.
      ring_color = ((millis() - s_flash_start_ms) / (2 * kRingBlinkPhaseMs)) % 2 == 0
                       ? kNotableMagenta
                       : kNotableGold;
      break;
    default:
      break;
  }

  for (int dy = -ro; dy <= ro; ++dy) {
    const int y = cy + dy;
    if (y < 0 || y >= h) {
      continue;
    }
    const int dy2 = dy * dy;
    const int xo2 = ro2 - dy2;
    if (xo2 < 0) {
      continue;
    }
    const int xo = static_cast<int>(sqrtf(static_cast<float>(xo2)));
    uint16_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w);

    if (dy2 >= ri2) {
      fillRowSpan(row, cx - xo, cx + xo, w, ring_color);
      continue;
    }
    const int xi = static_cast<int>(sqrtf(static_cast<float>(ri2 - dy2)));
    fillRowSpan(row, cx - xo, cx - xi - 1, w, ring_color);
    fillRowSpan(row, cx + xi + 1, cx + xo, w, ring_color);
  }
}

}  // namespace

void alertFlashStart(AlertFlashKind kind) {
  const unsigned long now = millis();
  s_flash_start_ms = now;
  s_flash_end_ms = now + kAlertFlashMs;
  // Do not downgrade an active military alert when a tracked alert follows it, and
  // never downgrade the notable "go look" ring.
  if (!s_active || kind == AlertFlashKind::Notable ||
      (kind == AlertFlashKind::Military && s_kind != AlertFlashKind::Notable)) {
    s_kind = kind;
  }
  s_active = true;
  Serial.printf("[alert] outer %s ring %lums thick=%dpx\n",
                s_kind == AlertFlashKind::Military     ? "red military"
                : s_kind == AlertFlashKind::Helicopter ? "blue helicopter"
                : s_kind == AlertFlashKind::Private    ? "white private"
                : s_kind == AlertFlashKind::Overhead   ? "yellow overhead"
                : s_kind == AlertFlashKind::Notable    ? "magenta/gold notable"
                                                       : "green tracked",
                static_cast<unsigned long>(kAlertFlashMs), kRingThicknessPx);
  // Bake into the next radar content rebuild + blit — keeps SPI off the hot path.
  radarDisplayInvalidateAircraft();
}

bool alertFlashActive() { return s_active; }

void alertFlashPaintBuffer(uint16_t* buf, int16_t w, int16_t h) {
  const unsigned long phase = (millis() - s_flash_start_ms) / kRingBlinkPhaseMs;
  if ((phase % 2UL) == 0) {
    paintRingBuffer(buf, w, h);
  }
}

bool alertFlashPoll() {
  if (!s_active) {
    return false;
  }

  if (static_cast<long>(millis() - s_flash_end_ms) >= 0) {
    s_active = false;
    s_flash_start_ms = 0;
    s_flash_end_ms = 0;
    Serial.println("[alert] outer red ring end");
    return true;
  }
  return false;
}

}  // namespace ui
