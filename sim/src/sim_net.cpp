// Wi-Fi and HTTP(S) for the simulator. Requests go out through the PC's network
// with WinHTTP (system TLS), so the firmware's real adsb.fi / weather / route /
// photo code runs against the live services.
// Windows headers first: Arduino.h's INPUT/OUTPUT macros break winuser.h.
// (winsock2 must come before windows.h)
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#undef INADDR_NONE

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "config.h"
#include "services/map_center.h"

WiFiClass WiFi;

// ---------------------------------------------------------------------------
// Fault injection, to exercise the firmware's offline handling:
//   --offline            Wi-Fi down and every HTTP request fails, for the whole run
//   --wifi-down A[:B]    WiFi.status() reports disconnected from A s to B s (B omitted = forever)
//   --feed-down A[:B]    Wi-Fi stays up but every HTTP request fails from A s to B s
// FLIGHTSCNR_SIM_OFFLINE=1 in the environment is the same as --offline.
// Times are seconds since the simulator started (the --tap/--shot clock).
// Read from the process command line here so sim_main.cpp needs no changes.
// ---------------------------------------------------------------------------
namespace {

struct FaultWindow {
  double from_s = -1;  // <0: never
  double to_s = -1;    // <0: until exit
  bool active(double now_s) const {
    return from_s >= 0 && now_s >= from_s && (to_s < 0 || now_s < to_s);
  }
};

struct NetFaults {
  FaultWindow wifi;
  FaultWindow http;
};

void parseWindow(const char* v, FaultWindow* w) {
  double a = 0, b = -1;
  const int n = sscanf(v, "%lf:%lf", &a, &b);
  if (n >= 1) {
    w->from_s = a;
    w->to_s = n == 2 ? b : -1;
  }
}

const NetFaults& netFaults() {
  static NetFaults f = [] {
    NetFaults out;
    const char* env = getenv("FLIGHTSCNR_SIM_OFFLINE");
    bool offline = env != nullptr && env[0] != '\0' && env[0] != '0';
    for (int i = 1; i < __argc; ++i) {
      const char* next = i + 1 < __argc ? __argv[i + 1] : nullptr;
      if (strcmp(__argv[i], "--offline") == 0) {
        offline = true;
      } else if (strcmp(__argv[i], "--wifi-down") == 0 && next != nullptr) {
        parseWindow(next, &out.wifi);
      } else if (strcmp(__argv[i], "--feed-down") == 0 && next != nullptr) {
        parseWindow(next, &out.http);
      }
    }
    if (offline) {
      out.wifi.from_s = 0;
      out.wifi.to_s = -1;
    }
    if (out.wifi.from_s >= 0 || out.http.from_s >= 0) {
      printf("[sim] fault injection: wifi-down %.0f..%.0f s, feed-down %.0f..%.0f s (-1 = off/forever)\n",
             out.wifi.from_s, out.wifi.to_s, out.http.from_s, out.http.to_s);
    }
    return out;
  }();
  return f;
}

double simNowS() { return static_cast<double>(millis()) / 1000.0; }

}  // namespace

// ---------------------------------------------------------------------------
// Fake aircraft (TEMPORARY test aid for the notable-aircraft alarm; sim only):
//   --fake-plane SEC:TYPE:CALLSIGN:REG[:MIL[:BEARING[:MILES[:HEADING[:KNOTS]]]]]
// From SEC seconds on, every adsb.fi response gets one extra aircraft that starts
// MILES (default 15) from home on BEARING (default 270, i.e. west of home) and flies
// HEADING (default 90) at KNOTS (default 240), offset 1 mi to the right of a direct
// line, at 6500 ft. MIL=1 sets dbFlags military. Repeat the flag for more planes.
// Example: --fake-plane 5:P3:NOAA42:N42RF
// ---------------------------------------------------------------------------
namespace {

struct FakePlane {
  double at_s = 0;
  char type[8] = "";
  char callsign[12] = "";
  char reg[16] = "";
  int mil = 0;
  double bearing = 270, miles = 15, heading = 90, knots = 240;
  char hex[8] = "";
};

const std::vector<FakePlane>& fakePlanes() {
  static std::vector<FakePlane> planes = [] {
    std::vector<FakePlane> out;
    for (int i = 1; i + 1 < __argc; ++i) {
      if (strcmp(__argv[i], "--fake-plane") != 0) continue;
      FakePlane p;
      std::string spec = __argv[i + 1];
      std::vector<std::string> f;
      size_t start = 0;
      while (true) {
        const size_t c = spec.find(':', start);
        f.push_back(spec.substr(start, c == std::string::npos ? std::string::npos : c - start));
        if (c == std::string::npos) break;
        start = c + 1;
      }
      if (f.size() < 4) continue;
      p.at_s = atof(f[0].c_str());
      snprintf(p.type, sizeof(p.type), "%s", f[1].c_str());
      snprintf(p.callsign, sizeof(p.callsign), "%s", f[2].c_str());
      snprintf(p.reg, sizeof(p.reg), "%s", f[3].c_str());
      if (f.size() > 4) p.mil = atoi(f[4].c_str());
      if (f.size() > 5) p.bearing = atof(f[5].c_str());
      if (f.size() > 6) p.miles = atof(f[6].c_str());
      if (f.size() > 7) p.heading = atof(f[7].c_str());
      if (f.size() > 8) p.knots = atof(f[8].c_str());
      snprintf(p.hex, sizeof(p.hex), "%06x", 0xfa0000u + static_cast<unsigned>(out.size()));
      printf("[sim] fake plane %s %s %s from %.0f s: %.0f mi at %.0f deg, hdg %.0f, %.0f kt\n",
             p.type, p.callsign, p.reg, p.at_s, p.miles, p.bearing, p.heading, p.knots);
      out.push_back(p);
    }
    return out;
  }();
  return planes;
}

void injectFakePlanes(const std::string& url, std::vector<uint8_t>* body) {
  const auto& planes = fakePlanes();
  if (planes.empty() || url.find(FLIGHTSCNR_ADSB_HOST) == std::string::npos) return;
  std::string s(body->begin(), body->end());
  const size_t ac = s.find("\"ac\":[");
  if (ac == std::string::npos) return;
  const double now = static_cast<double>(millis()) / 1000.0;
  const double home_lat = services::map_center::latitude();
  const double home_lon = services::map_center::longitude();
  const double kDeg = 3.14159265358979 / 180.0;
  std::string extra;
  for (const FakePlane& p : planes) {
    if (now < p.at_s) continue;
    const double flown_mi = p.knots * 1.15078 * (now - p.at_s) / 3600.0;
    // Start point, nudged 1 mi to the right of the heading so it passes near, not over.
    double e = p.miles * sin(p.bearing * kDeg) + cos(p.heading * kDeg) * 1.0;
    double n = p.miles * cos(p.bearing * kDeg) - sin(p.heading * kDeg) * 1.0;
    e += flown_mi * sin(p.heading * kDeg);
    n += flown_mi * cos(p.heading * kDeg);
    const double lat = home_lat + n / 69.0;
    const double lon = home_lon + e / (69.0 * cos(home_lat * kDeg));
    char obj[512];
    snprintf(obj, sizeof(obj),
             "{\"hex\":\"%s\",\"flight\":\"%-8s\",\"r\":\"%s\",\"t\":\"%s\",\"dbFlags\":%d,"
             "\"alt_baro\":6500,\"gs\":%.1f,\"track\":%.1f,\"true_heading\":%.1f,"
             "\"category\":\"A5\",\"squawk\":\"4512\",\"lat\":%.6f,\"lon\":%.6f,"
             "\"baro_rate\":0},",
             p.hex, p.callsign, p.reg, p.type, p.mil ? 1 : 0, p.knots, p.heading, p.heading,
             lat, lon);
    extra += obj;
  }
  if (extra.empty()) return;
  const size_t at = ac + 6;
  if (at < s.size() && s[at] == ']') extra.pop_back();  // empty list: no trailing comma
  s.insert(at, extra);
  body->assign(s.begin(), s.end());
}

}  // namespace

bool simWifiDown() { return netFaults().wifi.active(simNowS()); }
bool simHttpDown() {
  const double now = simNowS();
  return netFaults().wifi.active(now) || netFaults().http.active(now);
}

// ---------------------------------------------------------------------------
// WiFiClient: a buffer the HTTP response body is served from.
// ---------------------------------------------------------------------------
void WiFiClient::simLoad(std::vector<uint8_t>&& body) {
  buf_ = std::move(body);
  pos_ = 0;
  open_ = true;
}
int WiFiClient::available() { return static_cast<int>(buf_.size() - pos_); }
int WiFiClient::read() { return pos_ < buf_.size() ? buf_[pos_++] : -1; }
int WiFiClient::peek() { return pos_ < buf_.size() ? buf_[pos_] : -1; }
int WiFiClient::read(uint8_t* buf, size_t size) {
  const size_t n = std::min(size, buf_.size() - pos_);
  if (n > 0) memcpy(buf, buf_.data() + pos_, n);
  pos_ += n;
  return static_cast<int>(n);
}
size_t WiFiClient::readBytes(char* buffer, size_t length) {
  return static_cast<size_t>(read(reinterpret_cast<uint8_t*>(buffer), length));
}
void WiFiClient::stop() {
  buf_.clear();
  pos_ = 0;
  open_ = false;
}
uint8_t WiFiClient::connected() { return open_ && pos_ < buf_.size() ? 1 : 0; }

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
wl_status_t WiFiClass::status() { return simWifiDown() ? WL_DISCONNECTED : WL_CONNECTED; }
int8_t WiFiClass::RSSI() { return -52; }
String WiFiClass::SSID() { return String("Simulator (PC network)"); }
IPAddress WiFiClass::localIP() { return simWifiDown() ? IPAddress(0, 0, 0, 0) : IPAddress(127, 0, 0, 1); }
uint8_t* WiFiClass::macAddress(uint8_t* mac) {
  const uint8_t m[6] = {0x02, 0x00, 0x00, 0x51, 0x4D, 0x01};
  memcpy(mac, m, 6);
  return mac;
}

int WiFiClass::hostByName(const char* host, IPAddress& out) {
  if (simHttpDown()) return 0;
  static std::once_flag wsa_once;
  std::call_once(wsa_once, [] {
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
  });
  addrinfo hints = {};
  hints.ai_family = AF_INET;
  addrinfo* res = nullptr;
  if (getaddrinfo(host, nullptr, &hints, &res) != 0 || res == nullptr) return 0;
  const auto* sin = reinterpret_cast<const sockaddr_in*>(res->ai_addr);
  out = IPAddress(static_cast<uint32_t>(sin->sin_addr.s_addr));
  freeaddrinfo(res);
  return 1;
}

// ---------------------------------------------------------------------------
// HTTPClient over WinHTTP
// ---------------------------------------------------------------------------
namespace {

std::wstring widen(const String& s) {
  if (s.length() == 0) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
  std::wstring w(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
  w.resize(static_cast<size_t>(n - 1));
  return w;
}

String narrow(const wchar_t* w, DWORD chars) {
  const int n = WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(chars), nullptr, 0, nullptr,
                                    nullptr);
  std::string s(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, static_cast<int>(chars), &s[0], n, nullptr, nullptr);
  return String(s.c_str());
}

HINTERNET session() {
  static HINTERNET s = WinHttpOpen(L"FlightScnr-sim", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
  return s;
}

}  // namespace

bool HTTPClient::begin(WiFiClient& client, const String& url) {
  client_ = &client;
  url_ = url;
  size_ = -1;
  headers_.clear();
  resp_headers_.clear();
  return url.startsWith("http://") || url.startsWith("https://");
}

bool HTTPClient::begin(const String& url) { return begin(own_client_, url); }

bool HTTPClient::begin(WiFiClient& client, const String& host, uint16_t port, const String& uri,
                       bool https) {
  String url = https ? "https://" : "http://";
  url += host;
  url += ":";
  url += String(port);
  url += uri;
  return begin(client, url);
}

void HTTPClient::end() {
  headers_.clear();
  size_ = -1;
}

bool HTTPClient::connected() { return client_ != nullptr && client_->connected(); }

void HTTPClient::addHeader(const String& name, const String& value, bool first, bool replace) {
  (void)first;
  if (replace) {
    for (auto& h : headers_) {
      if (h.first.equalsIgnoreCase(name)) {
        h.second = value;
        return;
      }
    }
  }
  headers_.emplace_back(name, value);
}

void HTTPClient::collectHeaders(const char* keys[], size_t count) {
  collect_.clear();
  for (size_t i = 0; i < count; ++i) collect_.emplace_back(keys[i]);
}

String HTTPClient::header(const char* name) {
  for (const auto& h : resp_headers_) {
    if (h.first.equalsIgnoreCase(name)) return h.second;
  }
  return String();
}

bool HTTPClient::hasHeader(const char* name) { return header(name).length() > 0; }

int HTTPClient::GET() { return sendRequest("GET", nullptr, 0); }
int HTTPClient::POST(const String& payload) {
  return sendRequest("POST", reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length());
}
int HTTPClient::POST(const uint8_t* payload, size_t size) { return sendRequest("POST", payload, size); }
int HTTPClient::sendRequest(const char* type, const String& payload) {
  return sendRequest(type, reinterpret_cast<const uint8_t*>(payload.c_str()), payload.length());
}

int HTTPClient::sendRequest(const char* type, const uint8_t* payload, size_t size) {
  if (client_ == nullptr) return HTTPC_ERROR_NO_STREAM;
  client_->stop();
  size_ = -1;
  resp_headers_.clear();
  if (simHttpDown()) {
    // Like a dead uplink: a short stall, then a refused connection.
    delay(300);
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }

  const std::wstring wurl = widen(url_);
  URL_COMPONENTS uc = {};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256] = {};
  wchar_t path[4096] = {};
  uc.lpszHostName = host;
  uc.dwHostNameLength = 255;
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = 4095;
  wchar_t extra[4096] = {};
  uc.lpszExtraInfo = extra;
  uc.dwExtraInfoLength = 4095;
  if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) return HTTPC_ERROR_CONNECTION_REFUSED;
  std::wstring object = std::wstring(path) + extra;
  if (object.empty()) object = L"/";

  HINTERNET s = session();
  if (s == nullptr) return HTTPC_ERROR_CONNECTION_REFUSED;
  const int connect_ms = connect_timeout_ms_ > 0 ? connect_timeout_ms_ : 5000;
  const int io_ms = timeout_ms_ > 0 ? timeout_ms_ : 5000;
  HINTERNET conn = WinHttpConnect(s, host, uc.nPort, 0);
  if (conn == nullptr) return HTTPC_ERROR_CONNECTION_REFUSED;
  const bool https = uc.nScheme == INTERNET_SCHEME_HTTPS;
  const std::wstring verb = widen(String(type));
  HINTERNET req = WinHttpOpenRequest(conn, verb.c_str(), object.c_str(), nullptr,
                                     WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                     https ? WINHTTP_FLAG_SECURE : 0);
  if (req == nullptr) {
    WinHttpCloseHandle(conn);
    return HTTPC_ERROR_CONNECTION_REFUSED;
  }
  WinHttpSetTimeouts(req, connect_ms, connect_ms, io_ms, io_ms);
  if (follow_ == HTTPC_DISABLE_FOLLOW_REDIRECTS) {
    DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
  }
  // Ask for an uncompressed body, like the ESP32 HTTPClient does.
  std::wstring hdrs = L"User-Agent: " + widen(user_agent_) + L"\r\nAccept-Encoding: identity\r\n";
  for (const auto& h : headers_) {
    if (h.first.equalsIgnoreCase("User-Agent")) {
      continue;
    }
    hdrs += widen(h.first) + L": " + widen(h.second) + L"\r\n";
  }
  WinHttpAddRequestHeaders(req, hdrs.c_str(), static_cast<DWORD>(-1L),
                           WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE);

  const BOOL sent = WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                       const_cast<uint8_t*>(payload), static_cast<DWORD>(size),
                                       static_cast<DWORD>(size), 0);
  if (!sent || !WinHttpReceiveResponse(req, nullptr)) {
    const DWORD err = GetLastError();
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    return err == ERROR_WINHTTP_TIMEOUT ? HTTPC_ERROR_READ_TIMEOUT : HTTPC_ERROR_CONNECTION_REFUSED;
  }

  DWORD status = 0;
  DWORD len = sizeof(status);
  WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                      WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);

  for (const auto& key : collect_) {
    wchar_t buf[2048];
    DWORD blen = sizeof(buf);
    const std::wstring wkey = widen(key);
    if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CUSTOM, wkey.c_str(), buf, &blen,
                            WINHTTP_NO_HEADER_INDEX)) {
      resp_headers_.emplace_back(key, narrow(buf, blen / sizeof(wchar_t)));
    }
  }

  std::vector<uint8_t> body;
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0) break;
    const size_t old = body.size();
    body.resize(old + avail);
    DWORD got = 0;
    if (!WinHttpReadData(req, body.data() + old, avail, &got)) {
      body.resize(old);
      break;
    }
    body.resize(old + got);
  }
  WinHttpCloseHandle(req);
  WinHttpCloseHandle(conn);

  injectFakePlanes(url_.c_str(), &body);
  size_ = static_cast<int>(body.size());
  client_->simLoad(std::move(body));
  return static_cast<int>(status);
}

String HTTPClient::getString() {
  if (client_ == nullptr) return String();
  String s;
  const int n = client_->available();
  if (n <= 0) return s;
  std::string tmp(static_cast<size_t>(n), '\0');
  client_->readBytes(&tmp[0], static_cast<size_t>(n));
  s = tmp.c_str();
  return s;
}

int HTTPClient::writeToStream(Stream* stream) {
  if (client_ == nullptr || stream == nullptr) return HTTPC_ERROR_NO_STREAM;
  int total = 0;
  uint8_t buf[1024];
  for (;;) {
    const int n = client_->available();
    if (n <= 0) break;
    const size_t want = static_cast<size_t>(n) < sizeof(buf) ? static_cast<size_t>(n) : sizeof(buf);
    const size_t got = client_->readBytes(buf, want);
    if (got == 0) break;
    if (stream->write(buf, got) != got) return HTTPC_ERROR_STREAM_WRITE;
    total += static_cast<int>(got);
  }
  return total;
}

String HTTPClient::errorToString(int error) {
  switch (error) {
    case HTTPC_ERROR_CONNECTION_REFUSED: return "connection refused";
    case HTTPC_ERROR_NO_STREAM: return "no stream";
    case HTTPC_ERROR_READ_TIMEOUT: return "read Timeout";
    default: return String("error ") + String(error);
  }
}
