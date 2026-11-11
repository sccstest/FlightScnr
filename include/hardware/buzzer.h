#pragma once

#include <cstdint>

#include "hardware/chime_synth.h"
#include "hardware/pin_config.h"

namespace hardware {

/** Configure buzzer / haptic (call once from setup). */
void buzzerInit();

/** Load beep on/off and tone from flash. */
void buzzerBootLoad();

bool buzzerEnabled();
/** Current tone step A (quietest) through E (loudest). Piezo boards: beep tone; speaker
 *  boards: chime volume. On speaker boards buzzerEnabled() is "alert sound on". */
char buzzerToneLetter();

void buzzerSetEnabled(bool enabled);
#if FLIGHTSCNR_HAS_SPEAKER
/** One-touch mute: flip Alert Sound and save it like the settings page does. Unmuting
 *  plays a short confirm chirp; muting is silent. Returns the new state (true = on). */
bool buzzerToggleAlertSound();
#endif
/** Step beep tone A–E (no-op on haptic / Waveshare boards). */
void buzzerToneStep(int8_t delta);

/** Start a short non-blocking click if beeps/vibration are enabled. */
void buzzerClick();

/**
 * Play alert feedback: a triple beep on piezo boards, the matching chime on speaker
 * boards (FLIGHTSCNR_HAS_SPEAKER). Haptic boards use the screen flash only.
 */
void buzzerAlert(chime::Kind kind = chime::Kind::Tracked);

/** Speaker boards: play a chime at the current volume, even if alert sound is off (tests). */
void buzzerPlayChime(chime::Kind kind);

/** Stop an in-progress beep; call from loop. */
void buzzerPoll();

void saveBeepEnabledFromForm(const char* checkbox_value);
void saveBeepToneFromForm(const char* value);

}  // namespace hardware
