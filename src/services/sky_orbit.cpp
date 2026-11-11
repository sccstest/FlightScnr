#include "services/sky_orbit.h"

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)

#include <cmath>
#include <cstdlib>
#include <cstring>

namespace services::sky::orbit {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;
constexpr double kDeg = kPi / 180.0;

// WGS-72 (what the TLEs are fitted with).
constexpr double kRe = 6378.135;  // km
constexpr double kMu = 398600.8;  // km^3/s^2
constexpr double kJ2 = 0.001082616;
constexpr double kJ3 = -0.00000253881;
constexpr double kJ4 = -0.00000165597;
constexpr double kJ3oJ2 = kJ3 / kJ2;
constexpr double kX2o3 = 2.0 / 3.0;
const double kXke = 60.0 / std::sqrt(kRe * kRe * kRe / kMu);

double field(const char* line, int col1, int len) {
  // col1 is the 1-based TLE column.
  char buf[24];
  if (len >= static_cast<int>(sizeof(buf))) return 0.0;
  memcpy(buf, line + col1 - 1, static_cast<size_t>(len));
  buf[len] = '\0';
  return atof(buf);
}

/** "-12345-3" style: implied leading decimal point and a power-of-ten exponent. */
double impliedExp(const char* line, int col1) {
  char buf[16];
  memcpy(buf, line + col1 - 1, 8);
  buf[8] = '\0';
  const double sign = buf[0] == '-' ? -1.0 : 1.0;
  char mant[8] = {};
  memcpy(mant, buf + 1, 5);
  const double m = atof(mant) * 1e-5;
  const int e = atoi(buf + 6);
  return sign * m * std::pow(10.0, e);
}

bool checksumOk(const char* line) {
  int sum = 0;
  for (int i = 0; i < 68; ++i) {
    const char c = line[i];
    if (c >= '0' && c <= '9') sum += c - '0';
    if (c == '-') sum += 1;
  }
  return line[68] >= '0' && line[68] <= '9' && (sum % 10) == line[68] - '0';
}

double jday(int year, int mon, int day) {
  return 367.0 * year - std::floor(7.0 * (year + std::floor((mon + 9) / 12.0)) * 0.25) +
         std::floor(275.0 * mon / 9.0) + day + 1721013.5;
}

double gmstRad(double jd_ut1) {
  const double tut1 = (jd_ut1 - 2451545.0) / 36525.0;
  double t = -6.2e-6 * tut1 * tut1 * tut1 + 0.093104 * tut1 * tut1 +
             (876600.0 * 3600.0 + 8640184.812866) * tut1 + 67310.54841;  // seconds
  t = std::fmod(t * kDeg / 240.0, kTwoPi);
  if (t < 0.0) t += kTwoPi;
  return t;
}

/** Unit vector to the sun in the (true-of-date-ish) inertial frame. ~0.01 deg. */
void sunEci(double jd, double s[3]) {
  const double n = jd - 2451545.0;
  const double L = std::fmod(280.460 + 0.9856474 * n, 360.0);
  const double g = std::fmod(357.528 + 0.9856003 * n, 360.0) * kDeg;
  const double lambda = (L + 1.915 * std::sin(g) + 0.020 * std::sin(2.0 * g)) * kDeg;
  const double eps = (23.439 - 0.0000004 * n) * kDeg;
  s[0] = std::cos(lambda);
  s[1] = std::cos(eps) * std::sin(lambda);
  s[2] = std::sin(eps) * std::sin(lambda);
}

void observerEcef(double lat_deg, double lon_deg, double alt_m, double o[3]) {
  constexpr double a = 6378.137;
  constexpr double f = 1.0 / 298.257223563;
  const double e2 = f * (2.0 - f);
  const double phi = lat_deg * kDeg;
  const double lam = lon_deg * kDeg;
  const double sp = std::sin(phi);
  const double N = a / std::sqrt(1.0 - e2 * sp * sp);
  const double h = alt_m / 1000.0;
  o[0] = (N + h) * std::cos(phi) * std::cos(lam);
  o[1] = (N + h) * std::cos(phi) * std::sin(lam);
  o[2] = (N * (1.0 - e2) + h) * sp;
}

}  // namespace

double unixToJd(double unix_sec) { return unix_sec / 86400.0 + 2440587.5; }
double jdToUnix(double jd) { return (jd - 2440587.5) * 86400.0; }

bool parseTle(const char* l1, const char* l2, Tle* out) {
  if (l1 == nullptr || l2 == nullptr || out == nullptr) return false;
  if (strlen(l1) < 69 || strlen(l2) < 69 || l1[0] != '1' || l2[0] != '2') return false;
  if (!checksumOk(l1) || !checksumOk(l2)) return false;
  Tle t;
  int yy = static_cast<int>(field(l1, 19, 2));
  const int year = yy < 57 ? 2000 + yy : 1900 + yy;
  const double doy = field(l1, 21, 12);
  t.epoch_jd = jday(year, 1, 1) + doy - 1.0;
  t.bstar = impliedExp(l1, 54);
  t.incl = field(l2, 9, 8) * kDeg;
  t.raan = field(l2, 18, 8) * kDeg;
  char ecc[12] = "0.";
  memcpy(ecc + 2, l2 + 26, 7);
  ecc[9] = '\0';
  t.ecc = atof(ecc);
  t.argp = field(l2, 35, 8) * kDeg;
  t.mean_anom = field(l2, 44, 8) * kDeg;
  t.mean_motion = field(l2, 53, 11) * kTwoPi / 1440.0;
  if (t.mean_motion <= 0.0 || t.ecc >= 1.0) return false;
  *out = t;
  return true;
}

bool sgp4Init(const Tle& tle, Sgp4* s) {
  *s = Sgp4{};
  s->tle = tle;
  const double ecco = tle.ecc;
  const double inclo = tle.incl;
  const double argpo = tle.argp;
  const double mo = tle.mean_anom;
  const double bstar = tle.bstar;

  const double ss = 78.0 / kRe + 1.0;
  const double qzms2t = std::pow((120.0 - 78.0) / kRe, 4);

  // initl
  const double eccsq = ecco * ecco;
  const double omeosq = 1.0 - eccsq;
  const double rteosq = std::sqrt(omeosq);
  const double cosio = std::cos(inclo);
  const double cosio2 = cosio * cosio;
  const double ak = std::pow(kXke / tle.mean_motion, kX2o3);
  const double d1 = 0.75 * kJ2 * (3.0 * cosio2 - 1.0) / (rteosq * omeosq);
  double del = d1 / (ak * ak);
  const double adel = ak * (1.0 - del * del - del * (1.0 / 3.0 + 134.0 * del * del / 81.0));
  del = d1 / (adel * adel);
  const double no = tle.mean_motion / (1.0 + del);
  const double ao = std::pow(kXke / no, kX2o3);
  const double sinio = std::sin(inclo);
  const double po = ao * omeosq;
  const double con42 = 1.0 - 5.0 * cosio2;
  const double con41 = -con42 - cosio2 - cosio2;
  const double posq = po * po;
  const double rp = ao * (1.0 - ecco);
  if (2.0 * kPi / no >= 225.0) {
    return false;  // deep-space orbit: not supported (and not needed for the ISS)
  }

  s->no = no;
  s->ao = ao;
  s->con41 = con41;
  s->isimp = rp < (220.0 / kRe + 1.0) ? 1 : 0;

  double sfour = ss;
  double qzms24 = qzms2t;
  const double perige = (rp - 1.0) * kRe;
  if (perige < 156.0) {
    sfour = perige - 78.0;
    if (perige < 98.0) sfour = 20.0;
    qzms24 = std::pow((120.0 - sfour) / kRe, 4);
    sfour = sfour / kRe + 1.0;
  }
  const double pinvsq = 1.0 / posq;
  const double tsi = 1.0 / (ao - sfour);
  s->eta = ao * ecco * tsi;
  const double etasq = s->eta * s->eta;
  const double eeta = ecco * s->eta;
  const double psisq = std::fabs(1.0 - etasq);
  const double coef = qzms24 * std::pow(tsi, 4);
  const double coef1 = coef / std::pow(psisq, 3.5);
  const double cc2 = coef1 * no *
                     (ao * (1.0 + 1.5 * etasq + eeta * (4.0 + etasq)) +
                      0.375 * kJ2 * tsi / psisq * con41 * (8.0 + 3.0 * etasq * (8.0 + etasq)));
  s->cc1 = bstar * cc2;
  double cc3 = 0.0;
  if (ecco > 1.0e-4) cc3 = -2.0 * coef * tsi * kJ3oJ2 * no * sinio / ecco;
  s->x1mth2 = 1.0 - cosio2;
  s->cc4 = 2.0 * no * coef1 * ao * omeosq *
           (s->eta * (2.0 + 0.5 * etasq) + ecco * (0.5 + 2.0 * etasq) -
            kJ2 * tsi / (ao * psisq) *
                (-3.0 * con41 * (1.0 - 2.0 * eeta + etasq * (1.5 - 0.5 * eeta)) +
                 0.75 * s->x1mth2 * (2.0 * etasq - eeta * (1.0 + etasq)) * std::cos(2.0 * argpo)));
  s->cc5 = 2.0 * coef1 * ao * omeosq * (1.0 + 2.75 * (etasq + eeta) + eeta * etasq);
  const double cosio4 = cosio2 * cosio2;
  const double temp1 = 1.5 * kJ2 * pinvsq * no;
  const double temp2 = 0.5 * temp1 * kJ2 * pinvsq;
  const double temp3 = -0.46875 * kJ4 * pinvsq * pinvsq * no;
  s->mdot = no + 0.5 * temp1 * rteosq * con41 +
            0.0625 * temp2 * rteosq * (13.0 - 78.0 * cosio2 + 137.0 * cosio4);
  s->argpdot = -0.5 * temp1 * con42 + 0.0625 * temp2 * (7.0 - 114.0 * cosio2 + 395.0 * cosio4) +
               temp3 * (3.0 - 36.0 * cosio2 + 49.0 * cosio4);
  const double xhdot1 = -temp1 * cosio;
  s->nodedot = xhdot1 + (0.5 * temp2 * (4.0 - 19.0 * cosio2) + 2.0 * temp3 * (3.0 - 7.0 * cosio2)) * cosio;
  s->omgcof = bstar * cc3 * std::cos(argpo);
  s->xmcof = 0.0;
  if (ecco > 1.0e-4) s->xmcof = -kX2o3 * coef * bstar / eeta;
  s->nodecf = 3.5 * omeosq * xhdot1 * s->cc1;
  s->t2cof = 1.5 * s->cc1;
  const double den = std::fabs(cosio + 1.0) > 1.5e-12 ? (1.0 + cosio) : 1.5e-12;
  s->xlcof = -0.25 * kJ3oJ2 * sinio * (3.0 + 5.0 * cosio) / den;
  s->aycof = -0.5 * kJ3oJ2 * sinio;
  s->delmo = std::pow(1.0 + s->eta * std::cos(mo), 3);
  s->sinmao = std::sin(mo);
  s->x7thm1 = 7.0 * cosio2 - 1.0;
  if (s->isimp != 1) {
    const double cc1sq = s->cc1 * s->cc1;
    s->d2 = 4.0 * ao * tsi * cc1sq;
    const double temp = s->d2 * tsi * s->cc1 / 3.0;
    s->d3 = (17.0 * ao + sfour) * temp;
    s->d4 = 0.5 * temp * ao * tsi * (221.0 * ao + 31.0 * sfour) * s->cc1;
    s->t3cof = s->d2 + 2.0 * cc1sq;
    s->t4cof = 0.25 * (3.0 * s->d3 + s->cc1 * (12.0 * s->d2 + 10.0 * cc1sq));
    s->t5cof = 0.2 * (3.0 * s->d4 + 12.0 * s->cc1 * s->d3 + 6.0 * s->d2 * s->d2 +
                      15.0 * cc1sq * (2.0 * s->d2 + cc1sq));
  }
  s->ok = true;
  return true;
}

bool sgp4Propagate(const Sgp4& s, double t, double r_km[3]) {
  if (!s.ok) return false;
  const Tle& e = s.tle;
  const double xmdf = e.mean_anom + s.mdot * t;
  const double argpdf = e.argp + s.argpdot * t;
  const double nodedf = e.raan + s.nodedot * t;
  double argpm = argpdf;
  double mm = xmdf;
  const double t2 = t * t;
  double nodem = nodedf + s.nodecf * t2;
  double tempa = 1.0 - s.cc1 * t;
  double tempe = e.bstar * s.cc4 * t;
  double templ = s.t2cof * t2;
  if (s.isimp != 1) {
    const double delomg = s.omgcof * t;
    const double delmtemp = 1.0 + s.eta * std::cos(xmdf);
    const double delm = s.xmcof * (delmtemp * delmtemp * delmtemp - s.delmo);
    const double temp = delomg + delm;
    mm = xmdf + temp;
    argpm = argpdf - temp;
    const double t3 = t2 * t;
    const double t4 = t3 * t;
    tempa = tempa - s.d2 * t2 - s.d3 * t3 - s.d4 * t4;
    tempe = tempe + e.bstar * s.cc5 * (std::sin(mm) - s.sinmao);
    templ = templ + s.t3cof * t3 + t4 * (s.t4cof + t * s.t5cof);
  }
  double nm = s.no;
  double em = e.ecc;
  const double inclm = e.incl;
  if (nm <= 0.0) return false;
  const double am = std::pow(kXke / nm, kX2o3) * tempa * tempa;
  nm = kXke / std::pow(am, 1.5);
  em = em - tempe;
  if (em >= 1.0 || em < -0.001 || am < 0.95) return false;  // decayed / diverged
  if (em < 1.0e-6) em = 1.0e-6;
  mm = mm + s.no * templ;
  double xlm = mm + argpm + nodem;
  nodem = std::fmod(nodem, kTwoPi);
  argpm = std::fmod(argpm, kTwoPi);
  xlm = std::fmod(xlm, kTwoPi);
  mm = std::fmod(xlm - argpm - nodem, kTwoPi);

  const double sinip = std::sin(inclm);
  const double cosip = std::cos(inclm);
  const double ep = em;

  // Long-period periodics.
  const double axnl = ep * std::cos(argpm);
  double temp = 1.0 / (am * (1.0 - ep * ep));
  const double aynl = ep * std::sin(argpm) + temp * s.aycof;
  const double xl = mm + argpm + nodem + temp * s.xlcof * axnl;

  // Kepler's equation.
  const double u = std::fmod(xl - nodem, kTwoPi);
  double eo1 = u;
  double tem5 = 9999.9;
  double sineo1 = 0.0;
  double coseo1 = 0.0;
  for (int ktr = 1; std::fabs(tem5) >= 1.0e-12 && ktr <= 10; ++ktr) {
    sineo1 = std::sin(eo1);
    coseo1 = std::cos(eo1);
    tem5 = 1.0 - coseo1 * axnl - sineo1 * aynl;
    tem5 = (u - aynl * coseo1 + axnl * sineo1 - eo1) / tem5;
    if (std::fabs(tem5) >= 0.95) tem5 = tem5 > 0.0 ? 0.95 : -0.95;
    eo1 += tem5;
  }

  // Short-period periodics.
  const double ecose = axnl * coseo1 + aynl * sineo1;
  const double esine = axnl * sineo1 - aynl * coseo1;
  const double el2 = axnl * axnl + aynl * aynl;
  const double pl = am * (1.0 - el2);
  if (pl < 0.0) return false;
  const double rl = am * (1.0 - ecose);
  const double betal = std::sqrt(1.0 - el2);
  temp = esine / (1.0 + betal);
  const double sinu = am / rl * (sineo1 - aynl - axnl * temp);
  const double cosu = am / rl * (coseo1 - axnl + aynl * temp);
  double su = std::atan2(sinu, cosu);
  const double sin2u = (cosu + cosu) * sinu;
  const double cos2u = 1.0 - 2.0 * sinu * sinu;
  temp = 1.0 / pl;
  const double temp1 = 0.5 * kJ2 * temp;
  const double temp2 = temp1 * temp;
  const double mrt = rl * (1.0 - 1.5 * temp2 * betal * s.con41) + 0.5 * temp1 * s.x1mth2 * cos2u;
  su = su - 0.25 * temp2 * s.x7thm1 * sin2u;
  const double xnode = nodem + 1.5 * temp2 * cosip * sin2u;
  const double xinc = inclm + 1.5 * temp2 * cosip * sinip * cos2u;

  const double sinsu = std::sin(su);
  const double cossu = std::cos(su);
  const double snod = std::sin(xnode);
  const double cnod = std::cos(xnode);
  const double sini = std::sin(xinc);
  const double cosi = std::cos(xinc);
  const double xmx = -snod * cosi;
  const double xmy = cnod * cosi;
  const double ux = xmx * sinsu + cnod * cossu;
  const double uy = xmy * sinsu + snod * cossu;
  const double uz = sini * sinsu;
  if (mrt < 1.0) return false;  // below the surface
  r_km[0] = mrt * ux * kRe;
  r_km[1] = mrt * uy * kRe;
  r_km[2] = mrt * uz * kRe;
  return true;
}

bool lookAngles(const Sgp4& s, double unix_sec, double lat_deg, double lon_deg, double alt_m,
                Look* out) {
  const double jd = unixToJd(unix_sec);
  const double tsince = (jd - s.tle.epoch_jd) * 1440.0;
  double r[3];
  if (!sgp4Propagate(s, tsince, r)) return false;

  // Earth shadow (cylinder), in the inertial frame.
  double sun[3];
  sunEci(jd, sun);
  const double dot = r[0] * sun[0] + r[1] * sun[1] + r[2] * sun[2];
  bool sunlit = true;
  if (dot < 0.0) {
    const double px = r[0] - dot * sun[0];
    const double py = r[1] - dot * sun[1];
    const double pz = r[2] - dot * sun[2];
    sunlit = std::sqrt(px * px + py * py + pz * pz) > 6378.0;
  }

  // Inertial -> earth-fixed.
  const double th = gmstRad(jd);
  const double c = std::cos(th);
  const double sn = std::sin(th);
  const double rx = r[0] * c + r[1] * sn;
  const double ry = -r[0] * sn + r[1] * c;
  const double rz = r[2];
  const double sx = sun[0] * c + sun[1] * sn;
  const double sy = -sun[0] * sn + sun[1] * c;
  const double sz = sun[2];

  double o[3];
  observerEcef(lat_deg, lon_deg, alt_m, o);
  const double dx = rx - o[0];
  const double dy = ry - o[1];
  const double dz = rz - o[2];
  const double phi = lat_deg * kDeg;
  const double lam = lon_deg * kDeg;
  const double sp = std::sin(phi), cp = std::cos(phi), sl = std::sin(lam), cl = std::cos(lam);
  const double south = sp * cl * dx + sp * sl * dy - cp * dz;
  const double east = -sl * dx + cl * dy;
  const double up = cp * cl * dx + cp * sl * dy + sp * dz;
  const double range = std::sqrt(dx * dx + dy * dy + dz * dz);
  double az = std::atan2(east, -south) / kDeg;
  if (az < 0.0) az += 360.0;

  out->az_deg = static_cast<float>(az);
  out->el_deg = static_cast<float>(std::asin(up / range) / kDeg);
  out->range_km = static_cast<float>(range);
  out->sun_el_deg = static_cast<float>(std::asin(cp * cl * sx + cp * sl * sy + sp * sz) / kDeg);
  out->sunlit = sunlit;
  return true;
}

// --- Pass search -------------------------------------------------------------------

namespace {
constexpr int kCoarseStepS = 60;
constexpr int kFineStepS = 5;
constexpr float kVisibleMinElDeg = 10.0f;
constexpr float kDarkSunElDeg = -4.0f;   // visible window: sky dark enough to see it
constexpr float kNightSunElDeg = -6.0f;  // ...and properly dark at some point of the pass
}  // namespace

void PassFinder::begin(const Sgp4* sat, double lat, double lon, int64_t from, int64_t to) {
  sat_ = sat;
  lat_ = lat;
  lon_ = lon;
  t_ = from;
  to_ = to;
  Look l;
  prev_up_ = sat_ != nullptr && lookAngles(*sat_, static_cast<double>(t_), lat_, lon_, 0, &l) &&
             l.el_deg > 0.0f;
  if (prev_up_) {
    // Already mid-pass: scan it from a little earlier so its start is found.
    t_ -= 15 * 60;
    prev_up_ = false;
  }
}

bool PassFinder::scanPass(int64_t from) {
  Pass p{};
  bool any = false;
  bool was_up = false;
  bool ended = false;
  bool dark = false;
  for (int64_t t = from; t < from + 25 * 60; t += kFineStepS) {
    Look l;
    if (!lookAngles(*sat_, static_cast<double>(t), lat_, lon_, 0, &l)) break;
    if (l.el_deg > 0.0f) {
      was_up = true;
    } else if (was_up) {
      t_ = t;
      ended = true;
      break;
    }
    const bool vis = l.el_deg >= kVisibleMinElDeg && l.sunlit && l.sun_el_deg < kDarkSunElDeg;
    if (!vis) continue;
    if (l.sun_el_deg < kNightSunElDeg) dark = true;
    if (!any) {
      any = true;
      p.start = t;
      p.start_az = l.az_deg;
      p.peak_el = -90.0f;
    }
    p.end = t;
    p.end_az = l.az_deg;
    if (l.el_deg > p.peak_el) {
      p.peak_el = l.el_deg;
      p.peak = t;
      p.peak_az = l.az_deg;
    }
  }
  if (!ended) t_ = from + 25 * 60;
  prev_up_ = false;
  // Heavens-Above style: skip passes that only happen in bright twilight.
  if (any && dark) pass_ = p;
  return any && dark;
}

bool PassFinder::step(int max_steps) {
  if (sat_ == nullptr) {
    t_ = to_;
    return false;
  }
  for (int i = 0; i < max_steps && t_ < to_; ++i) {
    Look l;
    if (!lookAngles(*sat_, static_cast<double>(t_), lat_, lon_, 0, &l)) {
      t_ = to_;
      return false;
    }
    const bool up = l.el_deg > 0.0f;
    if (up && !prev_up_) {
      if (scanPass(t_ - kCoarseStepS)) return true;
      continue;
    }
    prev_up_ = up;
    t_ += kCoarseStepS;
  }
  return false;
}

const char* compass8(float az) {
  static const char* const kNames[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
  int i = static_cast<int>(std::floor((az + 22.5f) / 45.0f)) % 8;
  if (i < 0) i += 8;
  return kNames[i];
}

void bearingDistance(double lat1, double lon1, double lat2, double lon2, float* bearing,
                     float* dist) {
  const double p1 = lat1 * kDeg, p2 = lat2 * kDeg, dl = (lon2 - lon1) * kDeg;
  const double y = std::sin(dl) * std::cos(p2);
  const double x = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
  double b = std::atan2(y, x) / kDeg;
  if (b < 0) b += 360.0;
  const double a = std::sin((p2 - p1) / 2) * std::sin((p2 - p1) / 2) +
                   std::cos(p1) * std::cos(p2) * std::sin(dl / 2) * std::sin(dl / 2);
  if (bearing) *bearing = static_cast<float>(b);
  if (dist) *dist = static_cast<float>(6371.0 * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1 - a)));
}

}  // namespace services::sky::orbit

#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C
