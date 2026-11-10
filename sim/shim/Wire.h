#pragma once
// I2C for the sim: a fake GT9271 touch controller (driven by the mouse) answers
// at 0x5D; every other address NACKs. See sim/src/sim_input.cpp.
#include <vector>

#include "Arduino.h"

class TwoWire : public Stream {
 public:
  bool begin(int sda = -1, int scl = -1, uint32_t freq = 0);
  bool end() { return true; }
  void setClock(uint32_t) {}
  void beginTransmission(uint8_t addr);
  void beginTransmission(int addr) { beginTransmission(static_cast<uint8_t>(addr)); }
  uint8_t endTransmission(bool send_stop = true);
  size_t requestFrom(uint8_t addr, size_t len, bool send_stop = true);
  size_t requestFrom(int addr, int len) {
    return requestFrom(static_cast<uint8_t>(addr), static_cast<size_t>(len));
  }
  size_t write(uint8_t b) override;
  size_t write(const uint8_t* data, size_t n) override;
  using Print::write;
  int available() override;
  int read() override;
  int peek() override;
  void flush() override {}

 private:
  uint8_t tx_addr_ = 0;
  std::vector<uint8_t> tx_;
  std::vector<uint8_t> rx_;
  size_t rx_pos_ = 0;
  uint16_t reg_ = 0;
};

extern TwoWire Wire;
