#pragma once

#include "hardware/plane_gfx.h"

/**
 * Sky events UI (Waveshare 4C): the countdown banner along the top band of the radar,
 * the ISS marker on the radar edge during a pass, and the tap-through detail page.
 * Data comes from services/sky_events.h.
 */
namespace ui::sky {

/** Call every loop(). Repaints the banner strip on the radar when its text changes. */
void tick(bool radar_visible);

/** Draw the banner + ISS marker into the radar frame being composed. */
void drawRadarOverlay(PlaneGfx& gfx);

/** Area the banner can cover while one is shown (false when none is). Plane tags
 *  keep out of it. */
bool bannerBounds(int* x, int* y, int* w, int* h);

/** True when a radar tap at (x, y) lands on the banner. */
bool bannerHit(int x, int y);

/** Full-screen detail page for the event the banner shows. */
void detailDraw();
/** While the detail page is open: redraw once a second (countdowns). */
void detailTick();

}  // namespace ui::sky
