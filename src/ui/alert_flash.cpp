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
/** Radial thickness of the alert ring (px). Thin enough that it stays a frame and
 *  does not reach the tags, banner text and range labels near the edge. */
constexpr int kRingThicknessPx = ui::sc(12);

constexpr int kRingOuterRadiusPx = config::kDisplayWidth / 2 - kRingOuterInsetPx;
constexpr int kRingInnerRadiusPx = kRingOuterRadiusPx - kRingThicknessPx;

unsigned long s_flash_end_ms = 0;
unsigned long s_flash_start_ms = 0;
bool s_active = false;
AlertFlashKind s_kind = AlertFlashKind::Tracked;

/** Fill [x0, x1] of one row, clipped to [lo, hi]; pixels set in `keep` stay. */
void fillRowSpan(uint16_t* row, int x0, int x1, int lo, int hi, uint16_t color,
                 const uint8_t* keep) {
  if (x0 < lo) {
    x0 = lo;
  }
  if (x1 > hi) {
    x1 = hi;
  }
  for (int x = x0; x <= x1; ++x) {
    if (keep != nullptr && (keep[x >> 3] & (1u << (x & 7))) != 0) {
      continue;
    }
    row[x] = color;
  }
}

/** Solid annulus via horizontal spans into RAM — no SPI, no fillArc wedges. */
void paintRingBuffer(uint16_t* buf, int16_t w, int16_t h, int clip_x, int clip_y, int clip_w,
                     int clip_h, const uint8_t* keep_mask) {
  if (buf == nullptr || w <= 0 || h <= 0) {
    return;
  }
  const int lo_x = clip_x < 0 ? 0 : clip_x;
  const int hi_x = clip_x + clip_w > w ? w - 1 : clip_x + clip_w - 1;
  const int lo_y = clip_y < 0 ? 0 : clip_y;
  const int hi_y = clip_y + clip_h > h ? h - 1 : clip_y + clip_h - 1;
  if (hi_x < lo_x || hi_y < lo_y) {
    return;
  }
  const size_t mask_stride = (static_cast<size_t>(w) + 7) / 8;

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
    if (y < lo_y || y > hi_y) {
      continue;
    }
    const int dy2 = dy * dy;
    const int xo2 = ro2 - dy2;
    if (xo2 < 0) {
      continue;
    }
    const int xo = static_cast<int>(sqrtf(static_cast<float>(xo2)));
    uint16_t* row = buf + static_cast<size_t>(y) * static_cast<size_t>(w);
    const uint8_t* keep =
        keep_mask != nullptr ? keep_mask + static_cast<size_t>(y) * mask_stride : nullptr;

    if (dy2 >= ri2) {
      fillRowSpan(row, cx - xo, cx + xo, lo_x, hi_x, ring_color, keep);
      continue;
    }
    const int xi = static_cast<int>(sqrtf(static_cast<float>(ri2 - dy2)));
    fillRowSpan(row, cx - xo, cx - xi - 1, lo_x, hi_x, ring_color, keep);
    fillRowSpan(row, cx + xi + 1, cx + xo, lo_x, hi_x, ring_color, keep);
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
  alertFlashPaintBufferRect(buf, w, h, 0, 0, w, h, nullptr);
}

void alertFlashPaintBufferRect(uint16_t* buf, int16_t w, int16_t h, int clip_x, int clip_y,
                               int clip_w, int clip_h, const uint8_t* keep_mask) {
  if (alertFlashRingShown()) {
    paintRingBuffer(buf, w, h, clip_x, clip_y, clip_w, clip_h, keep_mask);
  }
}

bool alertFlashRingShown() {
  if (!s_active) {
    return false;
  }
  const unsigned long phase = (millis() - s_flash_start_ms) / kRingBlinkPhaseMs;
  return (phase % 2UL) == 0;
}

uint32_t alertFlashPhase() {
  if (!s_active) {
    return 0;
  }
  return static_cast<uint32_t>((millis() - s_flash_start_ms) / kRingBlinkPhaseMs) + 1U;
}

void alertFlashRingRadii(int* outer, int* inner) {
  *outer = kRingOuterRadiusPx;
  *inner = kRingInnerRadiusPx;
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
