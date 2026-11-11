#pragma once

#include <cstddef>
#include <cstdint>

/**
 * Alert chime synthesizer: short bell / marimba-style jingles rendered to 16-bit mono PCM.
 * Pure C++ (no hardware), shared by the Waveshare 4C speaker (ES8311) and the desktop
 * simulator (SDL audio). Each aircraft alert kind has its own pattern so the alert can be
 * told apart without looking at the screen.
 */
namespace hardware::chime {

enum class Kind : uint8_t {
  Tracked,     // watch list (callsign / type / registration): rising major arpeggio
  Military,    // two-tone horn call, low and firm
  Helicopter,  // fluttering rotor pulses, then a soft high note
  Private,     // gentle "ding-dong"
  Overhead,    // upward swoosh into a bright bell
  Emergency,   // urgent alternating hi-lo (squawk 7500/7600/7700)
  Click,       // very short UI tick
  Notable,     // "go outside and look": fast fanfare run up to a sparkling high chord
};

constexpr size_t kKindCount = 8;
constexpr uint32_t kSampleRate = 16000;
/** Longest chime is 1.5 s; buffers of this many samples hold any of them. */
constexpr size_t kMaxSamples = kSampleRate * 3 / 2;

/** Samples render() will produce for this chime. */
size_t length(Kind kind);

/**
 * Render a chime into out (mono, kSampleRate). gain 0..1 scales the output; 1.0 peaks
 * just under full scale. Returns samples written (min(length(kind), max_samples)).
 */
size_t render(Kind kind, float gain, int16_t* out, size_t max_samples);

/** Lower-case name ("military", "helicopter", ...), for logs and file names. */
const char* name(Kind kind);

}  // namespace hardware::chime
