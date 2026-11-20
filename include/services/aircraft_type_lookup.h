#pragma once

#include <cstddef>

namespace services::aircraft_type {

/** Resolve ICAO type designator (e.g. B738) to full description from flash table. */
bool lookupDescription(const char* icao_type, char* out, size_t out_len);

/** Radar tag: short name if <= 7 chars, otherwise ICAO code (e.g. E75L). */
void formatRadarTagLabel(const char* icao_type, char* out, size_t out_len);

/** Longest maker / model text formatMakeModel() produces (tags must stay narrow). */
constexpr size_t kMakeMaxChars = 12;
constexpr size_t kModelMaxChars = 12;

/**
 * Radar tag as maker + short model from the ICAO type table:
 * C172 -> "Cessna" / "172 Skyhawk", B738 -> "Boeing" / "737-800",
 * CL35 -> "Bombardier" / "Challenger". False if the type is unknown.
 */
bool formatMakeModel(const char* icao_type, char* make, size_t make_len, char* model,
                     size_t model_len);

}  // namespace services::aircraft_type
