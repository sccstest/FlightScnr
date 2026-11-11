#pragma once

#include <cstdint>

/** Rotary encoder, knob button, and CHSC5816 touch gestures. */
void inputInit();

/** Poll encoder rotation, knob press, and touch. Call frequently from loop. */
void inputPoll();

/**
 * Rotary encoder detent since last call.
 * @return +1 clockwise, -1 counter-clockwise, 0 if none.
 */
int8_t inputConsumeEncoderDelta();

/** True after knob short-press (no touch coordinates). */
bool inputConsumeKnobTap();

/** True once when the knob button is first pressed down. */
bool inputConsumeKnobPress();

/**
 * True after a screen tap (not a swipe). Optional touch position in display coords.
 */
bool inputConsumeScreenTap(int16_t* x, int16_t* y);

enum SwipeGesture : uint8_t {
  SwipeNone = 0,
  SwipeLeft,
  SwipeRight,
  SwipeUp,
  SwipeDown,
};

/** Swipe detected since last poll; SwipeNone if no swipe pending. */
SwipeGesture inputConsumeSwipe();

/**
 * Two-finger pinch steps since last call (multi-touch panels only; 0 elsewhere).
 * @return +1 per spread step (fingers apart = zoom in), -1 per squeeze step.
 */
int8_t inputConsumePinchStep();

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
/**
 * True once after a one-finger long-press (held still ~0.9 s). Fires while the finger
 * is still down; that touch then produces no tap or swipe. Position in display coords.
 */
bool inputConsumeLongPress(int16_t* x, int16_t* y);
#endif

/** Poll for knob long-press (Wi-Fi reset). May reboot the device. */
void inputPollLongPress();

/**
 * True once when the user released the knob after the Wi-Fi reset countdown UI
 * was shown (abort). Caller should redraw the active screen.
 */
bool inputConsumeWifiResetUiCancelled();

/** Drop queued knob/encoder/touch events (e.g. after idle timeout to radar). */
void inputDiscardPendingInteractions();
