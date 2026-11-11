#pragma once

#include <cstddef>
#include <cstdint>

#include "services/sky_orbit.h"

/**
 * Live "sky events" for the Waveshare 4C: rocket launches from Florida's Space Coast
 * (Cape Canaveral SFS / Kennedy Space Center) and visible ISS passes, each with a
 * go-outside chime. Nothing is collected: only the next few events are kept.
 *
 * - Launches: Launch Library 2 (The Space Devs, keyless free tier, ~15 req/h). Polled
 *   hourly, every 15 min in the last two hours and every 5 min around liftoff; the
 *   last reply is cached on LittleFS so a reboot doesn't spend another request.
 * - ISS: a TLE from CelesTrak (keyless) refreshed daily and cached on LittleFS;
 *   passes are predicted on the device (services/sky_orbit.h).
 *
 * The UI side (top banner on the radar, detail page, ISS edge marker) lives in
 * ui/sky_banner.h. 4C only.
 */
namespace services::sky {

enum class EventKind : uint8_t { None, Launch, Iss };

struct Launch {
  char id[16] = "";        // first chars of the LL2 UUID
  char rocket[28] = "";    // "Falcon 9"
  char mission[44] = "";   // "Starlink Group 12-5"
  char pad[40] = "";       // "SLC-40" (shortened from "Space Launch Complex 40")
  char site[40] = "";      // "Cape Canaveral SFS"
  char status[24] = "";    // "Go for Launch"
  uint8_t status_id = 0;   // LL2: 1 Go, 2 TBD, 3 Success, 4 Failure, 5 Hold, 6 In Flight, 7 Partial, 8 TBC
  uint8_t precision = 0;   // LL2 net_precision id (0 s, 1 min, 2 hour, >2 vaguer); 255 = unknown
  int64_t net = 0;         // unix s
  int64_t window_start = 0;
  int64_t window_end = 0;
  float pad_lat = 0;
  float pad_lon = 0;
  bool fake = false;       // simulator test event
};

/** What the top banner should show right now. */
struct Banner {
  EventKind kind = EventKind::None;
  char title[48] = "";    // small line, e.g. "FALCON 9 | STARLINK 12-5"
  char detail[48] = "";   // big line, e.g. "T-2:14:05" or "7:42 PM  NW to SE"
  bool urgent = false;    // alarm window / happening now: drawn filled
};

void bootLoad();
/** Call every loop(): schedules fetches, advances the pass search, fires chimes. */
void tick();

bool launchesEnabled();
bool issEnabled();
void toggleLaunches();
void toggleIss();
void saveFromForm(const char* launch_checkbox, const char* iss_checkbox);

/** Current banner (kind None = nothing to show). Recomputed by tick(). */
const Banner& banner();

/** The launch the banner / detail page is about (nullptr if none). */
const Launch* currentLaunch();
/** Upcoming visible ISS passes (soonest first) and how many. */
const orbit::Pass* issPasses(size_t* count);
/** During a pass: where to look right now. False when no pass is in progress. */
bool issLookNow(float* az_deg, float* el_deg);

/** Compass word ("east", "northeast", ...) for an azimuth. */
const char* compassWord(float az_deg);
/** Bearing (deg) and distance (km) from the radar center to the launch pad. */
void launchBearing(const Launch& l, float* bearing_deg, float* dist_km);

/** One-line status of the data sources for the detail page / logs. */
void sourceStatus(char* out, size_t len);

}  // namespace services::sky
