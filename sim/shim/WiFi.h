#pragma once
// Desktop Wi-Fi: always "connected" through the PC's own network.
#include <stdint.h>

#include <vector>

#include "Arduino.h"
#include "IPAddress.h"

typedef enum {
  WL_NO_SHIELD = 255,
  WL_IDLE_STATUS = 0,
  WL_NO_SSID_AVAIL = 1,
  WL_SCAN_COMPLETED = 2,
  WL_CONNECTED = 3,
  WL_CONNECT_FAILED = 4,
  WL_CONNECTION_LOST = 5,
  WL_DISCONNECTED = 6
} wl_status_t;

typedef enum { WIFI_OFF = 0, WIFI_STA = 1, WIFI_AP = 2, WIFI_AP_STA = 3 } wifi_mode_t;
#define WIFI_MODE_NULL WIFI_OFF
#define WIFI_MODE_STA WIFI_STA
#define WIFI_MODE_AP WIFI_AP
#define WIFI_MODE_APSTA WIFI_AP_STA
#define WIFI_SCAN_RUNNING (-1)
#define WIFI_SCAN_FAILED (-2)

/** A byte-buffer "socket": HTTPClient fills it with the whole response body. */
class WiFiClient : public Stream {
 public:
  WiFiClient() = default;
  virtual ~WiFiClient() = default;

  int available() override;
  int read() override;
  int read(uint8_t* buf, size_t size);
  int peek() override;
  void flush() override {}
  size_t write(uint8_t) override { return 1; }
  size_t write(const uint8_t*, size_t size) override { return size; }
  using Print::write;
  size_t readBytes(char* buffer, size_t length);
  size_t readBytes(uint8_t* buffer, size_t length) {
    return readBytes(reinterpret_cast<char*>(buffer), length);
  }
  virtual void stop();
  uint8_t connected();
  operator bool() { return connected(); }
  void setTimeout(uint32_t seconds_or_ms) { (void)seconds_or_ms; }
  void setNoDelay(bool) {}

  // Used by the sim HTTPClient.
  void simLoad(std::vector<uint8_t>&& body);

 protected:
  std::vector<uint8_t> buf_;
  size_t pos_ = 0;
  bool open_ = false;
};

class WiFiClass {
 public:
  wl_status_t status();
  bool isConnected() { return status() == WL_CONNECTED; }
  int8_t RSSI();
  String SSID();
  String SSID(uint8_t) { return SSID(); }
  String psk() { return String(); }
  String BSSIDstr() { return String("00:00:00:00:00:00"); }
  IPAddress localIP();
  IPAddress gatewayIP() { return IPAddress(192, 168, 1, 1); }
  IPAddress subnetMask() { return IPAddress(255, 255, 255, 0); }
  IPAddress dnsIP(uint8_t = 0) { return IPAddress(1, 1, 1, 1); }
  IPAddress softAPIP() { return IPAddress(4, 3, 2, 1); }
  uint8_t softAPgetStationNum() { return 0; }
  String macAddress() { return String("02:00:00:51:4D:01"); }
  uint8_t* macAddress(uint8_t* mac);
  int hostByName(const char* host, IPAddress& out);
  bool mode(wifi_mode_t) { return true; }
  wifi_mode_t getMode() { return WIFI_STA; }
  wl_status_t begin(const char* = nullptr, const char* = nullptr, int32_t = 0,
                    const uint8_t* = nullptr, bool = true) {
    return WL_CONNECTED;
  }
  bool disconnect(bool = false, bool = false) { return true; }
  bool reconnect() { return true; }
  void persistent(bool) {}
  bool setAutoReconnect(bool) { return true; }
  bool setHostname(const char*) { return true; }
  const char* getHostname() { return "flightscnr-sim"; }
  bool setSleep(bool) { return true; }
  bool setTxPower(int) { return true; }
  int16_t scanNetworks(bool = false, bool = false, bool = false, uint32_t = 300) { return 0; }
  int16_t scanComplete() { return 0; }
  void scanDelete() {}
  int32_t channel() { return 6; }
  int32_t channel(uint8_t) { return 6; }
  int32_t RSSI(uint8_t) { return -50; }
};

extern WiFiClass WiFi;
