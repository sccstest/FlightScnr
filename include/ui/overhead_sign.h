#pragma once

#include <cstddef>
#include <cstdint>

class PlaneGfx;

namespace ui {

/**
 * "What's flying over" sign (Waveshare 4C only): a two-line banner in the bottom
 * band of the round radar for the single closest aircraft, e.g.
 *   [logo] DAL1377 · A-321neo
 *          TPA → ATL · 13,000 ft · 4.2 mi
 * Live only: it reads the current ADS-B list (route/airline already filled from the
 * route cache during polls) and never triggers an API call of its own.
 */

/** Re-pick the closest aircraft and rebuild the text. True when what the sign shows
 *  (or whether it shows at all) changed since the last call. */
bool overheadSignUpdate();

/** Paint the current sign. fb is the RGB565 target buffer (width fb_w) for the
 *  semi-opaque backing and the logo; nullptr paints a solid backing without logo. */
void overheadSignDraw(PlaneGfx& gfx, uint16_t* fb, int fb_w, int fb_h);

/** Screen rect the sign can cover (full band, stable while enabled). False when hidden. */
bool overheadSignBounds(int* x, int* y, int* w, int* h);

/** True when (x, y) lands on the visible sign; copies that aircraft's Mode-S hex. */
bool overheadSignHitTest(int16_t x, int16_t y, char* hex_out, size_t hex_len);

}  // namespace ui
