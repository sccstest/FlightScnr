#pragma once
// Declarations only, so Arduino_GFX's SPI bus headers parse. The sim never drives SPI.
#include "Arduino.h"

#define SPI_MODE0 0
#define SPI_MODE1 1
#define SPI_MODE2 2
#define SPI_MODE3 3
#define MSBFIRST 1
#define LSBFIRST 0

class SPISettings {
 public:
  SPISettings() = default;
  SPISettings(uint32_t, uint8_t, uint8_t) {}
};

class SPIClass {
 public:
  void begin(int8_t = -1, int8_t = -1, int8_t = -1, int8_t = -1) {}
  void end() {}
  void beginTransaction(SPISettings) {}
  void endTransaction() {}
  uint8_t transfer(uint8_t d) { return d; }
  uint16_t transfer16(uint16_t d) { return d; }
  void transfer(void*, size_t) {}
  void write(uint8_t) {}
  void write16(uint16_t) {}
  void write32(uint32_t) {}
  void writeBytes(const uint8_t*, uint32_t) {}
  void writePixels(const void*, uint32_t) {}
};

extern SPIClass SPI;
