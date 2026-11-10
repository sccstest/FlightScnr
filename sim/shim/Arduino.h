#pragma once
// Desktop (Windows/MinGW) stand-in for the Arduino-ESP32 core, used only by the
// `sim` PlatformIO environment. Just enough API for FlightScnr's own code; the
// hardware layer (panel, touch, Wi-Fi, OTA) is replaced by sim/src/*.cpp.

#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef __cplusplus
#include <algorithm>
#include <cmath>
#endif

#include "esp_sim_common.h"
#include "pgmspace.h"

#ifndef ARDUINO
#define ARDUINO 10819
#endif
#define ESP_ARDUINO_VERSION_MAJOR 3
#define ESP_ARDUINO_VERSION_MINOR 3
#define ESP_ARDUINO_VERSION_PATCH 11

typedef bool boolean;
typedef uint8_t byte;
typedef uint16_t word;

#define HIGH 0x1
#define LOW 0x0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define INPUT_PULLDOWN 0x09
#define RISING 0x01
#define FALLING 0x02
#define CHANGE 0x03

#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105

#define radians(deg) ((deg) * DEG_TO_RAD)
#define degrees(rad) ((rad) * RAD_TO_DEG)
#define sq(x) ((x) * (x))
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#define lowByte(w) ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)((w) >> 8))
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define bit(b) (1UL << (b))

#define digitalPinToInterrupt(p) (p)
#define digitalPinToBitMask(p) (1UL << ((p) & 31))

#ifdef __cplusplus
using std::max;
using std::min;
using std::isinf;
using std::isnan;

long random(long howbig);
long random(long howsmall, long howbig);
long map(long x, long in_min, long in_max, long out_min, long out_max);
#endif

#ifdef __cplusplus
extern "C" {
#endif
unsigned long millis(void);
unsigned long micros(void);
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void yield(void);
void randomSeed(unsigned long seed);

void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t val);
int digitalRead(uint8_t pin);
void analogWrite(uint8_t pin, int value);
int analogRead(uint8_t pin);
void attachInterrupt(uint8_t pin, void (*isr)(void), int mode);
void detachInterrupt(uint8_t pin);

bool ledcAttach(uint8_t pin, uint32_t freq, uint8_t resolution);
bool ledcAttachChannel(uint8_t pin, uint32_t freq, uint8_t resolution, int8_t channel);
bool ledcWrite(uint8_t pin, uint32_t duty);
uint32_t ledcWriteTone(uint8_t pin, uint32_t freq);
uint32_t ledcChangeFrequency(uint8_t pin, uint32_t freq, uint8_t resolution);
bool ledcDetach(uint8_t pin);

void configTime(long gmtOffset_sec, int daylightOffset_sec, const char* server1,
                const char* server2 = nullptr, const char* server3 = nullptr);
void configTzTime(const char* tz, const char* server1, const char* server2 = nullptr,
                  const char* server3 = nullptr);
#ifdef __cplusplus
}
bool getLocalTime(struct tm* info, uint32_t ms = 5000);
#endif

#ifdef __cplusplus
inline void* ps_malloc(size_t n) { return malloc(n); }
inline void* ps_calloc(size_t n, size_t s) { return calloc(n, s); }
inline void* ps_realloc(void* p, size_t n) { return realloc(p, n); }

#include "HardwareSerial.h"
#include "Stream.h"
#include "WString.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

class EspClass {
 public:
  uint32_t getFreeHeap();
  uint32_t getMinFreeHeap();
  uint32_t getMaxAllocHeap();
  uint32_t getHeapSize();
  uint32_t getFreePsram();
  uint32_t getPsramSize();
  uint64_t getEfuseMac();
  uint32_t getChipId();
  const char* getSdkVersion();
  void restart();
};
extern EspClass ESP;

#include "stdlib_noniso.h"

// Task watchdog helpers (no watchdog on the desktop).
inline void disableCore0WDT() {}
inline void enableCore0WDT() {}
inline void disableCore1WDT() {}
inline void enableCore1WDT() {}
inline void disableLoopWDT() {}
inline void enableLoopWDT() {}
inline void feedLoopWDT() {}
#endif
