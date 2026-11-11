#pragma once

#include <cstddef>
#include <cstdint>

#include "config.h"

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)

/**
 * Live rain radar overlay for the 4C radar screen.
 *
 * Source: NOAA NEXRAD base-reflectivity mosaic (n0q, refreshed every 5 minutes)
 * served keyless by the Iowa Environmental Mesonet (Iowa State University) WMS:
 *   https://mesonet.agron.iastate.edu/cgi-bin/wms/nexrad/n0q.cgi
 * IEM states its materials are public domain and may be used freely; it asks
 * for credit to the Iowa Environmental Mesonet. Coverage is the continental US.
 *
 * The fetch, PNG decode and projection run on the ADS-B HTTPS worker; the loop
 * only swaps a finished frame in and blends it into the radar background sprite
 * (above the basemap, below the grid and planes).
 */
namespace services::rain {

/** Load the NVS preference (namespace "fs_rain"). */
void init();

bool enabled();
void setEnabled(bool on);
void toggleEnabled();
/** Persist the web settings checkbox ("T"/"on"/missing). */
void saveEnabledFromForm(const char* checkbox_value);

/**
 * Call every loop. Schedules fetches only while the radar is visible and Wi-Fi
 * is up (about every 5 minutes, sooner after the view changes), swaps in a
 * finished frame, and asks the radar to rebuild its background when the
 * overlay appears, changes or goes stale.
 */
void tick(bool radar_visible, bool wifi_up);

/**
 * Blend the current overlay into a kDisplayWidth² RGB565 (little-endian) buffer.
 * Returns false (and draws nothing) when disabled, stale, or for another view.
 */
bool blendInto(uint16_t* pixels, int w, int h);

/** "Rain 9:05 AM" for the frame blendInto would draw; false when none. */
bool labelText(char* buf, size_t len);

}  // namespace services::rain

#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C
