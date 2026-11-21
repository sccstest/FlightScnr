#pragma once

#include "services/route_info.h"

namespace services::route {

/**
 * True when both origin and dest are non-empty.
 * Pure, hardware-free (host-testable seam).
 */
bool routeEndpointsComplete(const RouteInfo& r);

/**
 * Whether a live paid-API step should terminate the detail waterfall.
 * Requires a complete origin/dest pair so airline-only hits do not block
 * later providers (FA / FR24 / adsbdb) from filling the route.
 */
bool shouldFinishLiveApiStep(const RouteInfo& r);

/**
 * Copy non-empty fields from `partial` into `dest` without overwriting
 * fields that are already set. Used when an earlier provider returns
 * airline-only data and the waterfall continues.
 */
void mergePartialRoute(RouteInfo* dest, const RouteInfo& partial);

/**
 * Great-circle distance in km from the plane at (plat, plon) to the path
 * origin -> dest: the cross-track distance while the plane is between the two
 * airports, otherwise the distance to the nearer end.
 */
float routeOffPathKm(float olat, float olon, float dlat, float dlon, float plat, float plon);

/** Allowed distance off the path: 100 km plus 10% of the route length. */
float routeOffPathLimitKm(float route_km);

/**
 * False when the plane is too far from the origin -> dest path for the route
 * to be its current flight (e.g. adsbdb's stale LAS-MSY for a plane over
 * Tampa). Static callsign databases go stale when airlines reuse numbers.
 */
bool routeFitsPosition(float olat, float olon, float dlat, float dlon, float plat, float plon,
                       float* off_km_out = nullptr);

}  // namespace services::route
