#pragma once

#include <cstdint>

class PlaneGfx;

namespace services::adsb {
struct Aircraft;
}

namespace ui::aircraft_icon {

/** Resolve Pi-style icon category id for an aircraft (ICAO type + heuristics). */
uint8_t resolveCategory(const services::adsb::Aircraft& aircraft);

/** True when a flash silhouette exists for this category id. */
bool hasIcon(uint8_t category);

/** Icon category is a (civil or military) helicopter. */
bool isHelicopterIcon(uint8_t category);
/** Icon category is typical private / general aviation: single or twin prop,
 *  business jet, glider, balloon. */
bool isGeneralAviationIcon(uint8_t category);

/** Draw tinted, heading-rotated silhouette. Returns false → caller should vector-fallback. */
bool draw(PlaneGfx& gfx, int cx, int cy, float heading_deg, uint16_t color, uint8_t category,
          int base_side_px);

}  // namespace ui::aircraft_icon
