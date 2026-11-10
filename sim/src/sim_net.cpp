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

#include <mutex>
#include <string>

WiFiClass WiFi;

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
wl_status_t WiFiClass::status() { return WL_CONNECTED; }
int8_t WiFiClass::RSSI() { return -52; }
String WiFiClass::SSID() { return String("Simulator (PC network)"); }
IPAddress WiFiClass::localIP() { return IPAddress(127, 0, 0, 1); }
uint8_t* WiFiClass::macAddress(uint8_t* mac) {
  const uint8_t m[6] = {0x02, 0x00, 0x00, 0x51, 0x4D, 0x01};
  memcpy(mac, m, 6);
  return mac;
}

int WiFiClass::hostByName(const char* host, IPAddress& out) {
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

String HTTPClient::errorToString(int error) {
  switch (error) {
    case HTTPC_ERROR_CONNECTION_REFUSED: return "connection refused";
    case HTTPC_ERROR_NO_STREAM: return "no stream";
    case HTTPC_ERROR_READ_TIMEOUT: return "read Timeout";
    default: return String("error ") + String(error);
  }
}
