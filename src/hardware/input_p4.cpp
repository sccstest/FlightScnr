// Touch-only input for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4C (GT9271, GT911 register map).
// Same public API and gesture rules as input.cpp; there is no encoder or knob button.
#include "hardware/pin_config.h"
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
#include "hardware/input.h"

#include <Arduino.h>
#include <Wire.h>

#include <cmath>
#include <cstdlib>

#include "hardware/buzzer.h"

namespace {

portMUX_TYPE s_input_mux = portMUX_INITIALIZER_UNLOCKED;
volatile int16_t s_tap_x = -1;
volatile int16_t s_tap_y = -1;
volatile SwipeGesture s_swipe_pending = SwipeNone;
volatile int8_t s_pinch_pending = 0;

uint8_t s_gt_addr = 0;
bool s_touch_ready = false;
bool s_touch_was_down = false;
bool s_touch_tracking = false;
int16_t s_touch_start_x = 0;
int16_t s_touch_start_y = 0;
int16_t s_touch_last_x = 0;
int16_t s_touch_last_y = 0;
unsigned long s_last_poll_ms = 0;

// Two-finger pinch: one step each time the finger spacing grows or shrinks by
// kPinchStepRatio from where the last step fired. A pinch never also counts as a
// tap or swipe; single-finger tracking resumes only after all fingers lift.
constexpr float kPinchStepRatio = 1.30f;
bool s_pinching = false;
bool s_pinch_used = false;
float s_pinch_base = 0.0f;
unsigned long s_pinch_end_ms = 0;
// Fingers rarely lift together; ignore taps briefly after a pinch ends.
constexpr unsigned long kPostPinchTapLockMs = 400;

// S3 boards use 70 px on a 360–390 px screen; keep the same physical swipe length.
constexpr int kSwipeMinPx = 70 * LCD_WIDTH / 360;
constexpr unsigned long kPollIntervalMs = 15;

constexpr uint16_t kGtRegStatus = 0x814E;
constexpr uint16_t kGtRegPoint1 = 0x814F;
constexpr uint16_t kGtRegProductId = 0x8140;

bool gtWriteReg(uint16_t reg, const uint8_t* data, size_t len) {
  Wire.beginTransmission(s_gt_addr);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  for (size_t i = 0; i < len; ++i) {
    Wire.write(data[i]);
  }
  return Wire.endTransmission() == 0;
}

bool gtReadReg(uint16_t reg, uint8_t* out, size_t len) {
  Wire.beginTransmission(s_gt_addr);
  Wire.write(static_cast<uint8_t>(reg >> 8));
  Wire.write(static_cast<uint8_t>(reg & 0xFF));
  if (Wire.endTransmission(false) != 0) {
    return false;
  }
  const size_t got = Wire.requestFrom(s_gt_addr, static_cast<uint8_t>(len));
  if (got < len) {
    return false;
  }
  for (size_t i = 0; i < len; ++i) {
    out[i] = static_cast<uint8_t>(Wire.read());
  }
  return true;
}

bool i2cPresent(uint8_t addr) {
  Wire.beginTransmission(addr);
  return Wire.endTransmission() == 0;
}

void queueSwipe(SwipeGesture gesture) {
  portENTER_CRITICAL(&s_input_mux);
  s_swipe_pending = gesture;
  portEXIT_CRITICAL(&s_input_mux);
}

void queueTap(int16_t x, int16_t y) {
  if (s_pinch_end_ms != 0 && millis() - s_pinch_end_ms < kPostPinchTapLockMs) {
    return;
  }
  portENTER_CRITICAL(&s_input_mux);
  s_tap_x = x;
  s_tap_y = y;
  portEXIT_CRITICAL(&s_input_mux);
}

void finishTouchGesture() {
  const int dx = s_touch_last_x - s_touch_start_x;
  const int dy = s_touch_last_y - s_touch_start_y;
  const int adx = std::abs(dx);
  const int ady = std::abs(dy);

  if (dx <= -kSwipeMinPx && ady * 2 < adx) {
    queueSwipe(SwipeLeft);
  } else if (dx >= kSwipeMinPx && ady * 2 < adx) {
    queueSwipe(SwipeRight);
  } else if (dy >= kSwipeMinPx && adx * 2 < ady) {
    queueSwipe(SwipeDown);
  } else if (dy <= -kSwipeMinPx && adx * 2 < ady) {
    queueSwipe(SwipeUp);
  } else {
    queueTap(s_touch_start_x, s_touch_start_y);
  }
}

void queuePinch(int8_t step) {
  portENTER_CRITICAL(&s_input_mux);
  const int v = s_pinch_pending + step;
  s_pinch_pending = static_cast<int8_t>(v > 8 ? 8 : (v < -8 ? -8 : v));
  portEXIT_CRITICAL(&s_input_mux);
}

void trackPinch(int16_t x0, int16_t y0, int16_t x1, int16_t y1) {
  const float dx = static_cast<float>(x1 - x0);
  const float dy = static_cast<float>(y1 - y0);
  const float d = sqrtf(dx * dx + dy * dy);
  if (!s_pinching) {
    s_pinching = true;
    s_pinch_used = true;
    s_pinch_base = d;
    s_touch_tracking = false;  // the first finger's touch is not a tap/swipe
    // Drop anything the first finger may already have queued.
    portENTER_CRITICAL(&s_input_mux);
    s_tap_x = -1;
    s_tap_y = -1;
    s_swipe_pending = SwipeNone;
    portEXIT_CRITICAL(&s_input_mux);
    return;
  }
  if (s_pinch_base < 1.0f) {
    s_pinch_base = d;
    return;
  }
  if (d >= s_pinch_base * kPinchStepRatio) {
    queuePinch(1);
    s_pinch_base = d;
    hardware::buzzerClick();
  } else if (d <= s_pinch_base / kPinchStepRatio) {
    queuePinch(-1);
    s_pinch_base = d;
    hardware::buzzerClick();
  }
}

void initTouchGt911() {
  Wire.begin(IIC_SDA, IIC_SCL, 400000);
  delay(200);  // GT9271 power-on settle (Waveshare example waits the same)
  if (i2cPresent(GT911_SLAVE_ADDRESS)) {
    s_gt_addr = GT911_SLAVE_ADDRESS;
  } else if (i2cPresent(GT911_SLAVE_ADDRESS_ALT)) {
    s_gt_addr = GT911_SLAVE_ADDRESS_ALT;
  } else {
    Serial.println("Touch: GT9271 not found at 0x5D or 0x14");
    s_touch_ready = false;
    return;
  }
  char id[5] = {};
  gtReadReg(kGtRegProductId, reinterpret_cast<uint8_t*>(id), 4);
  Serial.printf("Touch: GT%s at 0x%02X\n", id, s_gt_addr);
  s_touch_ready = true;
}

void pollTouchGt911() {
  uint8_t status = 0;
  if (!gtReadReg(kGtRegStatus, &status, 1)) {
    return;
  }
  if ((status & 0x80) == 0) {
    return;  // no new frame yet; keep last state
  }
  const uint8_t points = status & 0x0F;
  bool down = points > 0 && points <= 10;
  int16_t x = 0;
  int16_t y = 0;
  int16_t x2 = 0;
  int16_t y2 = 0;
  if (down) {
    // Point records are 8 bytes each: id, x lo/hi, y lo/hi, size lo/hi, reserved.
    uint8_t p[16] = {};
    const size_t want = points >= 2 ? 16 : 8;
    if (gtReadReg(kGtRegPoint1, p, want)) {
      x = static_cast<int16_t>(p[1] | (p[2] << 8));
      y = static_cast<int16_t>(p[3] | (p[4] << 8));
      x2 = static_cast<int16_t>(p[9] | (p[10] << 8));
      y2 = static_cast<int16_t>(p[11] | (p[12] << 8));
    } else {
      down = false;
    }
  }
  const uint8_t clear = 0;
  gtWriteReg(kGtRegStatus, &clear, 1);

  if (down && points >= 2) {
    trackPinch(x, y, x2, y2);
    s_touch_was_down = true;
    return;
  }
  if (s_pinch_used) {
    // Ignore the leftover finger(s) of a pinch until everything lifts.
    if (!down) {
      s_pinch_used = false;
      s_pinching = false;
      s_touch_was_down = false;
      s_pinch_end_ms = millis();
    }
    return;
  }

  if (down && !s_touch_was_down) {
    s_touch_start_x = x;
    s_touch_start_y = y;
    s_touch_last_x = x;
    s_touch_last_y = y;
    s_touch_tracking = true;
    hardware::buzzerClick();
  } else if (down && s_touch_tracking) {
    s_touch_last_x = x;
    s_touch_last_y = y;
  } else if (!down && s_touch_was_down && s_touch_tracking) {
    finishTouchGesture();
    s_touch_tracking = false;
  }
  s_touch_was_down = down;
}

}  // namespace

void inputInit() { initTouchGt911(); }

void inputPoll() {
  if (!s_touch_ready) {
    return;
  }
  const unsigned long now = millis();
  if (now - s_last_poll_ms < kPollIntervalMs) {
    return;
  }
  s_last_poll_ms = now;
  pollTouchGt911();
}

int8_t inputConsumeEncoderDelta() { return 0; }
bool inputConsumeKnobTap() { return false; }
bool inputConsumeKnobPress() { return false; }

bool inputConsumeScreenTap(int16_t* x, int16_t* y) {
  portENTER_CRITICAL(&s_input_mux);
  const bool tap = s_tap_x >= 0 && s_tap_y >= 0;
  if (tap) {
    if (x != nullptr) *x = s_tap_x;
    if (y != nullptr) *y = s_tap_y;
    s_tap_x = -1;
    s_tap_y = -1;
  }
  portEXIT_CRITICAL(&s_input_mux);
  return tap;
}

int8_t inputConsumePinchStep() {
  portENTER_CRITICAL(&s_input_mux);
  const int8_t step = s_pinch_pending;
  s_pinch_pending = 0;
  portEXIT_CRITICAL(&s_input_mux);
  return step;
}

SwipeGesture inputConsumeSwipe() {
  portENTER_CRITICAL(&s_input_mux);
  const SwipeGesture swipe = s_swipe_pending;
  s_swipe_pending = SwipeNone;
  portEXIT_CRITICAL(&s_input_mux);
  return swipe;
}

void inputDiscardPendingInteractions() {
  portENTER_CRITICAL(&s_input_mux);
  s_swipe_pending = SwipeNone;
  s_pinch_pending = 0;
  s_tap_x = -1;
  s_tap_y = -1;
  portEXIT_CRITICAL(&s_input_mux);
}

// No knob: Wi-Fi wipe is available from the web settings page.
void inputPollLongPress() {}
bool inputConsumeWifiResetUiCancelled() { return false; }

#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C
