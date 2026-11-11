#pragma once

#include <cstddef>
#include <cstdint>

#include "services/adsb_client.h"

namespace ui {

/** Connection state shown as a small badge on the radar. */
enum class RadarLinkStatus : uint8_t {
  Ok = 0,
  NoWifi,    ///< "No Wi-Fi": the router link is down.
  FeedDown,  ///< "Flight data offline": Wi-Fi is up but adsb.fi polls keep failing.
};

/** Set the badge state; the radar repaints the badge on its next frame when it changes. */
void radarDisplaySetLinkStatus(RadarLinkStatus status);
RadarLinkStatus radarDisplayLinkStatus();

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
/** One-touch mute: show the centered "Sound on" / "Sound off" toast for ~1.5 s. */
void radarDisplayShowSoundToast(bool sound_on);
/** Expire the toast and repaint the muted icon when Alert Sound changes. Call from loop. */
void radarDisplayTickSoundOverlay();
#endif
/** Repaint this screen rect on the next radar frame (overlays such as the sky banner). */
void radarDisplayInvalidateRect(int x, int y, int w, int h);

/** Draw the static sonar/radar grid (black disc, green overlay, labels). */
void radarDisplayDraw();

/** Compose grid + aircraft + sweep in RAM and push one frame to the panel. */
void radarDisplayRefreshSweep();

/** Note ADS-B aircraft changes; panel updates on the next sweep frame. */
void radarDisplayRefreshAircraft();

/** Count of aircraft currently inside the outer ring, i.e. drawn as full
 *  airplanes rather than beyond-ring edge blips. Used by auto-idle so the radar
 *  only reappears once a blip actually enters the visible scope. */
size_t radarDisplayInRangeAircraftCount();

/** Count of in-range aircraft that would be drawn as full planes (respects
 *  alert-hide). Excludes beyond-ring edge blips. Used for idle-clock transitions. */
size_t radarDisplayVisibleAircraftCount();

/** True if aircraft is inside the outer range ring (drawn as a plane, not an edge blip). */
bool radarDisplayIsInRange(const services::adsb::Aircraft& ac);

/** Debug probes for [radar] resume diagnostics (kRadarResumeDebug). */
bool radarDisplayDebugBgReady();
bool radarDisplayDebugContentReady();
bool radarDisplayDebugContentBaseValid();

/** Force aircraft layer to redraw on the next sweep frame. */
void radarDisplayInvalidateAircraft();

/**
 * Request background rebuild to pick up a new/cleared basemap. Retries on the
 * sweep path until JPEG decode is allowed (HTTPS idle + enough heap).
 */
void radarDisplayInvalidateBasemap();

/** Drop offscreen radar sprites to relieve heap pressure (rebuilt on next draw). */
void radarDisplayReleasePressureSprites();

/** Grid + rose preview for facing adjust (no aircraft/sweep). Always shows cardinals. */
void radarDisplayDrawOrientationPreview();

}  // namespace ui
