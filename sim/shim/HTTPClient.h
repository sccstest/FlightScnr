#pragma once
// HTTPClient over WinHTTP (sim only). Requests are synchronous; the whole body is
// downloaded and then served through the WiFiClient passed to begin().
#include <utility>
#include <vector>

#include "WiFi.h"

#define HTTP_CODE_OK 200
#define HTTP_CODE_NO_CONTENT 204
#define HTTP_CODE_MOVED_PERMANENTLY 301
#define HTTP_CODE_FOUND 302
#define HTTP_CODE_NOT_MODIFIED 304
#define HTTP_CODE_BAD_REQUEST 400
#define HTTP_CODE_UNAUTHORIZED 401
#define HTTP_CODE_FORBIDDEN 403
#define HTTP_CODE_NOT_FOUND 404
#define HTTP_CODE_TOO_MANY_REQUESTS 429
#define HTTP_CODE_INTERNAL_SERVER_ERROR 500

#define HTTPC_ERROR_CONNECTION_REFUSED (-1)
#define HTTPC_ERROR_SEND_HEADER_FAILED (-2)
#define HTTPC_ERROR_SEND_PAYLOAD_FAILED (-3)
#define HTTPC_ERROR_NOT_CONNECTED (-4)
#define HTTPC_ERROR_CONNECTION_LOST (-5)
#define HTTPC_ERROR_NO_STREAM (-6)
#define HTTPC_ERROR_NO_HTTP_SERVER (-7)
#define HTTPC_ERROR_TOO_LESS_RAM (-8)
#define HTTPC_ERROR_ENCODING (-9)
#define HTTPC_ERROR_STREAM_WRITE (-10)
#define HTTPC_ERROR_READ_TIMEOUT (-11)

typedef enum {
  HTTPC_DISABLE_FOLLOW_REDIRECTS,
  HTTPC_STRICT_FOLLOW_REDIRECTS,
  HTTPC_FORCE_FOLLOW_REDIRECTS
} followRedirects_t;

class HTTPClient {
 public:
  HTTPClient() = default;
  ~HTTPClient() { end(); }

  bool begin(WiFiClient& client, const String& url);
  bool begin(const String& url);
  bool begin(WiFiClient& client, const String& host, uint16_t port, const String& uri = "/",
             bool https = false);
  void end();
  bool connected();

  void setReuse(bool) {}
  void setUserAgent(const String& ua) { user_agent_ = ua; }
  void setTimeout(uint16_t ms) { timeout_ms_ = ms; }
  void setConnectTimeout(int32_t ms) { connect_timeout_ms_ = ms; }
  void setFollowRedirects(followRedirects_t f) { follow_ = f; }
  void useHTTP10(bool = true) {}
  void setAuthorization(const char*, const char*) {}
  void addHeader(const String& name, const String& value, bool first = false,
                 bool replace = true);
  void collectHeaders(const char* keys[], size_t count);
  String header(const char* name);
  bool hasHeader(const char* name);

  int GET();
  int POST(const String& payload);
  int POST(const uint8_t* payload, size_t size);
  int sendRequest(const char* type, const String& payload);
  int sendRequest(const char* type, const uint8_t* payload = nullptr, size_t size = 0);

  int getSize() { return size_; }
  WiFiClient* getStreamPtr() { return client_; }
  WiFiClient& getStream() { return *client_; }
  String getString();
  /** Writes the (already de-chunked) body to stream; negative when the stream refuses.
   *  Like the core, returns HTTPC_ERROR_STREAM_WRITE (-10) on a short write. */
  int writeToStream(Stream* stream);
  static String errorToString(int error);

 private:
  WiFiClient* client_ = nullptr;
  WiFiClient own_client_;
  String url_;
  String user_agent_ = "ESP32HTTPClient";
  std::vector<std::pair<String, String>> headers_;
  std::vector<String> collect_;
  std::vector<std::pair<String, String>> resp_headers_;
  uint16_t timeout_ms_ = 5000;
  int32_t connect_timeout_ms_ = 5000;
  followRedirects_t follow_ = HTTPC_DISABLE_FOLLOW_REDIRECTS;
  int size_ = -1;
};
