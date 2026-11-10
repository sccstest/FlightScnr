// Arduino core basics for the desktop simulator: time, Serial, GPIO/LEDC no-ops, ESP info.
#include <Arduino.h>
#include <IPAddress.h>

#include <chrono>
#include <cstdio>
#include <mutex>
#include <random>
#include <thread>

#include "hardware/pin_config.h"
#include "sim_state.h"

namespace {
const auto kStart = std::chrono::steady_clock::now();
std::mutex s_serial_mutex;
std::mt19937 s_rng(12345);
}  // namespace

extern "C" unsigned long millis(void) {
  return static_cast<unsigned long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::steady_clock::now() - kStart)
                                        .count());
}

extern "C" unsigned long micros(void) {
  return static_cast<unsigned long>(std::chrono::duration_cast<std::chrono::microseconds>(
                                        std::chrono::steady_clock::now() - kStart)
                                        .count());
}

extern "C" void delay(uint32_t ms) {
  sim::checkQuit();
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

extern "C" void delayMicroseconds(uint32_t us) {
  std::this_thread::sleep_for(std::chrono::microseconds(us));
}

extern "C" void yield(void) { std::this_thread::yield(); }

// Arduino's WString uses utoa(), which MinGW's CRT lacks.
extern "C" char* utoa(unsigned value, char* str, int base) { return _ultoa(value, str, base); }

extern "C" void randomSeed(unsigned long seed) { s_rng.seed(seed); }
long random(long howbig) {
  if (howbig <= 0) return 0;
  return static_cast<long>(s_rng() % static_cast<unsigned long>(howbig));
}
long random(long howsmall, long howbig) {
  if (howsmall >= howbig) return howsmall;
  return howsmall + random(howbig - howsmall);
}
long map(long x, long in_min, long in_max, long out_min, long out_max) {
  if (in_max == in_min) return out_min;
  return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

// --- GPIO / PWM: nothing attached on the desktop. The backlight PWM is mirrored
// into the window brightness so the brightness setting is visible.
extern "C" void pinMode(uint8_t, uint8_t) {}
extern "C" void digitalWrite(uint8_t, uint8_t) {}
extern "C" int digitalRead(uint8_t) { return HIGH; }
extern "C" void analogWrite(uint8_t, int) {}
extern "C" int analogRead(uint8_t) { return 0; }
extern "C" void attachInterrupt(uint8_t, void (*)(void), int) {}
extern "C" void detachInterrupt(uint8_t) {}
extern "C" bool ledcAttach(uint8_t, uint32_t, uint8_t) { return true; }
extern "C" bool ledcAttachChannel(uint8_t, uint32_t, uint8_t, int8_t) { return true; }
extern "C" bool ledcWrite(uint8_t pin, uint32_t duty) {
#ifdef LCD_BL
  if (pin == LCD_BL) sim::setBacklight(static_cast<uint8_t>(duty > 255 ? 255 : duty));
#else
  (void)pin;
  (void)duty;
#endif
  return true;
}
extern "C" uint32_t ledcWriteTone(uint8_t, uint32_t freq) { return freq; }
extern "C" uint32_t ledcChangeFrequency(uint8_t, uint32_t freq, uint8_t) { return freq; }
extern "C" bool ledcDetach(uint8_t) { return true; }

// --- Time: the PC clock is already correct, so NTP setup is a no-op and local
// time is the PC's own time zone (the device would use the radar center's zone).
extern "C" void configTime(long, int, const char*, const char*, const char*) {}
extern "C" void configTzTime(const char*, const char*, const char*, const char*) {}
bool getLocalTime(struct tm* info, uint32_t) {
  const time_t now = time(nullptr);
  localtime_s(info, &now);
  return true;
}

// --- Serial -> console.
HardwareSerial Serial;
void HardwareSerial::flush() { fflush(stdout); }
size_t HardwareSerial::write(uint8_t c) {
  std::lock_guard<std::mutex> lock(s_serial_mutex);
  fputc(c, stdout);
  if (c == '\n') fflush(stdout);
  return 1;
}
size_t HardwareSerial::write(const uint8_t* buffer, size_t size) {
  std::lock_guard<std::mutex> lock(s_serial_mutex);
  fwrite(buffer, 1, size, stdout);
  fflush(stdout);
  return size;
}

// --- ESP: report healthy heap numbers so the firmware's low-memory recovery paths stay idle.
EspClass ESP;
uint32_t EspClass::getFreeHeap() { return 260000; }
uint32_t EspClass::getMinFreeHeap() { return 240000; }
uint32_t EspClass::getMaxAllocHeap() { return 120000; }
uint32_t EspClass::getHeapSize() { return 400000; }
uint32_t EspClass::getFreePsram() { return 30u * 1024u * 1024u; }
uint32_t EspClass::getPsramSize() { return 32u * 1024u * 1024u; }
uint64_t EspClass::getEfuseMac() { return 0x014D5300000002ULL; }
uint32_t EspClass::getChipId() { return 0x51A1; }
const char* EspClass::getSdkVersion() { return "sim"; }
void EspClass::restart() { esp_restart(); }

extern "C" esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }
extern "C" void esp_restart(void) {
  Serial.println("[sim] esp_restart() — the device would reboot here; closing the simulator.");
  fflush(stdout);
  sim::requestQuit();
  std::exit(0);
}
extern "C" uint32_t esp_random(void) { return static_cast<uint32_t>(s_rng()); }
extern "C" void esp_fill_random(void* buf, size_t len) {
  auto* p = static_cast<uint8_t*>(buf);
  for (size_t i = 0; i < len; ++i) p[i] = static_cast<uint8_t>(s_rng());
}
extern "C" uint32_t esp_get_free_heap_size(void) { return ESP.getFreeHeap(); }
extern "C" uint32_t esp_get_minimum_free_heap_size(void) { return ESP.getMinFreeHeap(); }

extern "C" const char* esp_err_to_name(esp_err_t code) {
  switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "ESP_ERR_UNKNOWN";
  }
}

extern "C" size_t heap_caps_get_free_size(uint32_t caps) {
  return (caps & MALLOC_CAP_SPIRAM) ? 30u * 1024u * 1024u : ESP.getFreeHeap();
}
extern "C" size_t heap_caps_get_largest_free_block(uint32_t caps) {
  return (caps & MALLOC_CAP_SPIRAM) ? 16u * 1024u * 1024u : ESP.getMaxAllocHeap();
}
extern "C" size_t heap_caps_get_minimum_free_size(uint32_t caps) {
  return heap_caps_get_free_size(caps);
}
extern "C" size_t heap_caps_get_total_size(uint32_t caps) {
  return (caps & MALLOC_CAP_SPIRAM) ? 32u * 1024u * 1024u : ESP.getHeapSize();
}

const IPAddress INADDR_NONE(0, 0, 0, 0);
size_t IPAddress::printTo(Print& p) const { return p.print(toString()); }
