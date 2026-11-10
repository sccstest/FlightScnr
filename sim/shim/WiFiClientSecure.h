#pragma once
// TLS is done by WinHTTP in the sim HTTPClient; this client only holds the body.
#include "WiFi.h"

struct sslclient_context {
  int socket = -1;
};

class WiFiClientSecure : public WiFiClient {
 public:
  WiFiClientSecure() : sslclient(new sslclient_context()) {}
  ~WiFiClientSecure() override { delete sslclient; }
  WiFiClientSecure(const WiFiClientSecure&) = delete;
  WiFiClientSecure& operator=(const WiFiClientSecure&) = delete;

  void setInsecure() {}
  void setCACert(const char*) {}
  void setCACertBundle(const uint8_t*) {}
  void setHandshakeTimeout(unsigned long) {}
  void stop() override { WiFiClient::stop(); }

 protected:
  sslclient_context* sslclient;
};

typedef WiFiClientSecure NetworkClientSecure;
