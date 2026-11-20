#pragma once

#include <cstdint>

namespace ui {

enum class AlertFlashKind : uint8_t {
  Tracked,
  Military,
  Helicopter,
  Private,
  Overhead,
  Notable,  // "go outside and look": magenta / gold alternating ring (4C)
};

/** Start a 10s blinking ring: red military, blue helicopter, white private,
 *  green for other tracked (watch-list / emergency) aircraft. */
void alertFlashStart(AlertFlashKind kind);

/** True while the red ring flash window is active. */
bool alertFlashActive();

/**
 * Paint the alert ring into an RGB565 framebuffer (radar content sprite).
 * Call from the radar rebuild path so the ring rides along with normal blits —
 * no per-frame SPI cost that would stall the sweep.
 */
void alertFlashPaintBuffer(uint16_t* buf, int16_t w, int16_t h);

/**
 * Same, limited to one rectangle. Pixels whose bit is set in `keep_mask` (1 bit per
 * pixel, rows of (w + 7) / 8 bytes, LSB = leftmost) are left alone, so labels baked
 * into the background stay readable on top of the ring. `keep_mask` may be null.
 */
void alertFlashPaintBufferRect(uint16_t* buf, int16_t w, int16_t h, int clip_x, int clip_y,
                               int clip_w, int clip_h, const uint8_t* keep_mask);

/** True while the ring is in the "on" half of its blink. */
bool alertFlashRingShown();

/** Changes whenever the ring's look changes (each 500 ms blink step); 0 when idle. */
uint32_t alertFlashPhase();

/** Outer / inner radius (px from the panel center) the ring paints between. */
void alertFlashRingRadii(int* outer, int* inner);

/**
 * Call once per loop. Does not draw. Returns true when the flash ends (caller
 * should redraw the UI to clear the ring).
 */
bool alertFlashPoll();

}  // namespace ui
