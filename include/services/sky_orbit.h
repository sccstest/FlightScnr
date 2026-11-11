#pragma once

#include <cstdint>

/**
 * Small near-earth SGP4 propagator plus the bits of astronomy needed to predict
 * visible ISS passes on the device (no pass-prediction API, no key).
 *
 * SGP4 follows Vallado et al., "Revisiting Spacetrack Report #3" (AIAA 2006-6753),
 * near-earth branch only (orbital period < 225 min, which covers the ISS), WGS-72
 * constants. TEME -> Earth-fixed uses GMST only (no polar motion / nutation), which
 * is well under 0.1 deg for a ground observer. Pure C++, no Arduino dependencies.
 */
namespace services::sky::orbit {

struct Tle {
  double epoch_jd = 0.0;    // Julian date (UTC) of the element set
  double bstar = 0.0;       // drag term (1/earth radii)
  double incl = 0.0;        // rad
  double raan = 0.0;        // rad
  double ecc = 0.0;
  double argp = 0.0;        // rad
  double mean_anom = 0.0;   // rad
  double mean_motion = 0.0; // rad/min (Kozai)
};

/** Parse the two element lines. Checks the line numbers and checksums. */
bool parseTle(const char* line1, const char* line2, Tle* out);

/** Initialized SGP4 state for one element set. */
struct Sgp4 {
  bool ok = false;
  Tle tle;
  int isimp = 0;
  double no = 0, ao = 0, con41 = 0, x1mth2 = 0, x7thm1 = 0, cc1 = 0, cc4 = 0, cc5 = 0,
         d2 = 0, d3 = 0, d4 = 0, delmo = 0, eta = 0, argpdot = 0, omgcof = 0, sinmao = 0,
         t2cof = 0, t3cof = 0, t4cof = 0, t5cof = 0, xlcof = 0, aycof = 0, mdot = 0,
         nodedot = 0, nodecf = 0, xmcof = 0;
};

bool sgp4Init(const Tle& tle, Sgp4* s);
/** Position (km, TEME) at minutes since the TLE epoch. False on decay / error. */
bool sgp4Propagate(const Sgp4& s, double tsince_min, double r_km[3]);

/** Unix seconds (UTC) <-> Julian date. */
double unixToJd(double unix_sec);
double jdToUnix(double jd);

/** Where the ISS (or any satellite) is as seen from the observer. */
struct Look {
  float az_deg = 0;    // 0 = north, clockwise
  float el_deg = 0;    // above the horizon
  float range_km = 0;
  float sun_el_deg = 0;  // sun elevation at the observer
  bool sunlit = false;   // satellite outside the earth's shadow
};

bool lookAngles(const Sgp4& s, double unix_sec, double lat_deg, double lon_deg,
                double alt_m, Look* out);

/** One visible pass: the part where the satellite is sunlit, above 10 deg and the
 *  sky is dark enough (sun below -4 deg; matches Heavens-Above). */
struct Pass {
  int64_t start = 0;  // unix s, visible part
  int64_t peak = 0;
  int64_t end = 0;
  float start_az = 0;
  float peak_az = 0;
  float end_az = 0;
  float peak_el = 0;  // max elevation within the visible part
};

/** Incremental pass search so it can run a slice per loop() on the device. */
class PassFinder {
 public:
  void begin(const Sgp4* sat, double lat, double lon, int64_t from_unix, int64_t to_unix);
  /** Do up to max_steps coarse steps. Returns true when a visible pass was found
   *  (read it with pass()); call again to continue. done() once the range is covered. */
  bool step(int max_steps);
  bool done() const { return t_ >= to_; }
  const Pass& pass() const { return pass_; }

 private:
  bool scanPass(int64_t rise_hint);
  const Sgp4* sat_ = nullptr;
  double lat_ = 0, lon_ = 0;
  int64_t t_ = 0, to_ = 0;
  bool prev_up_ = false;
  Pass pass_{};
};

/** "N", "NE", ... for an azimuth in degrees. */
const char* compass8(float az_deg);

/** Initial great-circle bearing (deg) and distance (km) between two points. */
void bearingDistance(double lat1, double lon1, double lat2, double lon2, float* bearing_deg,
                     float* dist_km);

}  // namespace services::sky::orbit
