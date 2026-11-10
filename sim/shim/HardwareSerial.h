#pragma once
// Serial -> the simulator's console window (stdout).
#include "Stream.h"

class HardwareSerial : public Stream {
 public:
  void begin(unsigned long baud, uint32_t config = 0, int8_t rx = -1, int8_t tx = -1) {
    (void)baud;
    (void)config;
    (void)rx;
    (void)tx;
  }
  void end() {}
  void setTxTimeoutMs(uint32_t) {}
  void setDebugOutput(bool) {}
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override;
  size_t write(uint8_t c) override;
  size_t write(const uint8_t* buffer, size_t size) override;
  using Print::write;
  operator bool() const { return true; }
};

typedef HardwareSerial HWCDC;
extern HardwareSerial Serial;
