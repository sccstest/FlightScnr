#pragma once

#include "services/adsb_client.h"

namespace services::alert {

/**
 * Quick-glance traffic class (radar icon color and per-class alerts).
 *  Military   — adsb.fi database military flag
 *  Helicopter — ADS-B emitter category A7, or a helicopter icon type
 *  Private    — light/small/glider/balloon or a single/twin prop / business-jet type,
 *               unless the callsign is an airline flight number (e.g. AAL576)
 *  Airline    — everything else (airliners, cargo, regional jets, unknown)
 */
enum class TrafficClass : uint8_t { Airline, Military, Helicopter, Private };
TrafficClass trafficClass(const services::adsb::Aircraft& ac);

/** Color radar icons by TrafficClass (default on for the 4C, off on the S3 boards). */
bool typeColorsEnabled();

/**
 * Predicted closest approach to the radar center (home): true when the aircraft will
 * pass within kOverheadRadiusMi in the next kOverheadLeadSec, or is overhead now and
 * still closing. Straight-line from current track and ground speed. Optional outputs:
 * seconds until closest approach and the predicted miss distance in miles.
 */
constexpr float kOverheadRadiusMi = 2.0f;
constexpr float kOverheadLeadSec = 180.0f;
/** "Overhead (any)" only counts aircraft at or below this (you can see / hear them). */
constexpr int kOverheadMaxAltFt = 10000;
bool approachingOverhead(const services::adsb::Aircraft& ac, float* t_sec = nullptr,
                         float* miss_mi = nullptr);

/** Class/watch alerts fire when the aircraft is approaching overhead instead of when
 *  it enters the radar ring (emergency squawks always fire immediately). */
bool alertOnApproachEnabled();
/** Alert for any low aircraft about to pass overhead (yellow ring). */
bool overheadAnyEnabled();
void toggleAlertOnApproach();
void toggleOverheadAny();
void saveApproachFromForm(const char* approach_checkbox, const char* overhead_checkbox);
bool helicopterAlertEnabled();
bool privateAlertEnabled();

/** On-device "Alerts" settings page toggles (persisted immediately). */
void toggleTypeColors();
void toggleMilitaryAlert();
void toggleEmergencyAlert();
void toggleHelicopterAlert();
void togglePrivateAlert();
/** Web settings form for the two new class alerts (checkbox "T" = on). */
void saveClassAlertsFromForm(const char* heli_checkbox, const char* private_checkbox,
                             const char* type_colors_checkbox);

void bootLoad();

/** Check aircraft list for new alert-worthy entries; fires buzzer if matched.
 *  Alerts on rising edge: alert-target enters the visible radar ring. Leaving
 *  the ring (or a range change) clears memory so re-entry alerts again. */
void checkNewAircraft(const services::adsb::Aircraft* list, size_t count);

/** Call after the radar range changes so in-range alert aircraft can fire again. */
void notifyRangeChanged();

/** Call when returning to the radar screen so on-screen alert aircraft vibe again. */
void notifyRadarResumed();

/** True if this aircraft should be drawn with alert highlight on radar. */
bool isHighlighted(const services::adsb::Aircraft& ac);

/** Pulse phase for flashing alert icons (true = bright, false = dim). Cycles ~500ms. */
bool pulsePhase();

bool militaryAlertEnabled();
bool emergencyAlertEnabled();
bool hideNonAlertedEnabled();
bool watchCallsignsEnabled();
size_t watchCallsignCount();
bool watchTypesEnabled();
size_t watchTypeCount();
bool watchRegsEnabled();
size_t watchRegCount();

/** Comma-separated watch list for settings form (e.g. ACA739,UAL123). */
void watchCallsignsFormatted(char* out, size_t out_len);

/** Comma-separated ICAO type watch list (e.g. B738,A333). */
void watchTypesFormatted(char* out, size_t out_len);

/** Comma-separated registration / tail watch list (e.g. N2136U,CS-TPQ). */
void watchRegsFormatted(char* out, size_t out_len);

void saveFromForm(const char* mil_checkbox, const char* emrg_checkbox,
                  const char* hide_checkbox, const char* watch_callsigns,
                  bool update_watch_callsigns, const char* watch_types,
                  bool update_watch_types, const char* watch_regs,
                  bool update_watch_regs);

}  // namespace services::alert
