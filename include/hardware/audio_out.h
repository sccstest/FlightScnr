#pragma once

#include "hardware/chime_synth.h"

/**
 * Speaker output hook for boards with FLIGHTSCNR_HAS_SPEAKER (Waveshare 4C).
 *
 * Implemented per platform:
 *   - device: src/hardware/audio_out_p4.cpp (ES8311 codec + I2S + PA, FreeRTOS task)
 *   - desktop simulator: sim/src/sim_audio.cpp (SDL audio queue)
 * Both render with the shared chime synthesizer (hardware/chime_synth.h).
 */
namespace hardware::audio_out {

/** Bring up the codec / audio device. Safe to call once from setup(); false = no audio. */
bool init();

/**
 * Start playing a chime without blocking. Replaces whatever is playing. gain 0..1.
 * Thread-safe. Returns false if audio is not available.
 */
bool playChime(chime::Kind kind, float gain);

}  // namespace hardware::audio_out
