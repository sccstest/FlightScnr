#pragma once
// Declaration-only WebServer so modules that expose download endpoints compile.
// The settings web server itself is not part of the simulator.
#include "FS.h"
#include "WString.h"

class WebServer {
 public:
  explicit WebServer(int port = 80) { (void)port; }
  void send(int, const char* = nullptr, const String& = String()) {}
  void send(int code, const char* type, const char* content) { send(code, type, String(content)); }
  void sendHeader(const String&, const String&, bool = false) {}
  template <typename T>
  size_t streamFile(T&, const String&, int = 200) { return 0; }
};
