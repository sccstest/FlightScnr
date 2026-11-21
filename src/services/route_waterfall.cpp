#include "services/route_waterfall.h"

#include <cmath>
#include <cstring>

namespace services::route {

bool routeEndpointsComplete(const RouteInfo& r) {
  return r.origin[0] != '\0' && r.dest[0] != '\0';
}

bool shouldFinishLiveApiStep(const RouteInfo& r) { return routeEndpointsComplete(r); }

void mergePartialRoute(RouteInfo* dest, const RouteInfo& partial) {
  if (dest == nullptr) {
    return;
  }
  if (dest->airline[0] == '\0' && partial.airline[0] != '\0') {
    std::strncpy(dest->airline, partial.airline, sizeof(dest->airline) - 1);
    dest->airline[sizeof(dest->airline) - 1] = '\0';
  }
  if (dest->airline_icao[0] == '\0' && partial.airline_icao[0] != '\0') {
    std::strncpy(dest->airline_icao, partial.airline_icao, sizeof(dest->airline_icao) - 1);
    dest->airline_icao[sizeof(dest->airline_icao) - 1] = '\0';
  }
  if (dest->origin[0] == '\0' && partial.origin[0] != '\0') {
    std::strncpy(dest->origin, partial.origin, sizeof(dest->origin) - 1);
    dest->origin[sizeof(dest->origin) - 1] = '\0';
  }
  if (dest->dest[0] == '\0' && partial.dest[0] != '\0') {
    std::strncpy(dest->dest, partial.dest, sizeof(dest->dest) - 1);
    dest->dest[sizeof(dest->dest) - 1] = '\0';
  }
}

namespace {

constexpr double kEarthKm = 6371.0;
constexpr double kDegToRad = 3.14159265358979323846 / 180.0;

double angularDist(double lat1, double lon1, double lat2, double lon2) {
  const double dlat = lat2 - lat1;
  const double dlon = lon2 - lon1;
  const double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                   std::cos(lat1) * std::cos(lat2) * std::sin(dlon / 2) * std::sin(dlon / 2);
  return 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

double bearing(double lat1, double lon1, double lat2, double lon2) {
  const double dlon = lon2 - lon1;
  return std::atan2(std::sin(dlon) * std::cos(lat2),
                    std::cos(lat1) * std::sin(lat2) - std::sin(lat1) * std::cos(lat2) * std::cos(dlon));
}

}  // namespace

float routeOffPathKm(float olat, float olon, float dlat, float dlon, float plat, float plon) {
  const double la1 = olat * kDegToRad, lo1 = olon * kDegToRad;
  const double la2 = dlat * kDegToRad, lo2 = dlon * kDegToRad;
  const double lap = plat * kDegToRad, lop = plon * kDegToRad;

  const double d12 = angularDist(la1, lo1, la2, lo2);
  const double d1p = angularDist(la1, lo1, lap, lop);
  if (d12 < 1e-9) {
    return static_cast<float>(d1p * kEarthKm);
  }
  const double dtheta = bearing(la1, lo1, lap, lop) - bearing(la1, lo1, la2, lo2);
  const double xt = std::asin(std::sin(d1p) * std::sin(dtheta));
  double c = std::cos(d1p) / std::cos(xt);
  c = c > 1.0 ? 1.0 : (c < -1.0 ? -1.0 : c);
  double at = std::acos(c);
  if (std::cos(dtheta) < 0) {
    at = -at;
  }
  double off;
  if (at < 0) {
    off = d1p;
  } else if (at > d12) {
    off = angularDist(la2, lo2, lap, lop);
  } else {
    off = std::fabs(xt);
  }
  return static_cast<float>(off * kEarthKm);
}

float routeOffPathLimitKm(float route_km) { return 100.0f + 0.10f * route_km; }

bool routeFitsPosition(float olat, float olon, float dlat, float dlon, float plat, float plon,
                       float* off_km_out) {
  const float off = routeOffPathKm(olat, olon, dlat, dlon, plat, plon);
  if (off_km_out != nullptr) {
    *off_km_out = off;
  }
  const float route_km = static_cast<float>(
      angularDist(olat * kDegToRad, olon * kDegToRad, dlat * kDegToRad, dlon * kDegToRad) *
      kEarthKm);
  return off <= routeOffPathLimitKm(route_km);
}

}  // namespace services::route
