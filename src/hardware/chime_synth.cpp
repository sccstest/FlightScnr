#include "hardware/chime_synth.h"

#include <cmath>

namespace hardware::chime {
namespace {

constexpr float kTwoPi = 6.28318530718f;

// Note frequencies (Hz). The 4C's small speaker has little below ~400 Hz, so every
// fundamental stays at A4 or above.
constexpr float A4 = 440.00f;
constexpr float B4 = 493.88f;
constexpr float C5 = 523.25f;
constexpr float D5 = 587.33f;
constexpr float E5 = 659.26f;
constexpr float G5 = 783.99f;
constexpr float A5 = 880.00f;
constexpr float B5 = 987.77f;
constexpr float C6 = 1046.50f;
constexpr float E6 = 1318.51f;

enum class Timbre : uint8_t {
  Bell,  // struck: fundamental + quickly fading 2nd/3rd partials
  Horn,  // held: brighter odd/even partials with a sustain, then release
  Soft,  // nearly pure sine
};

struct Note {
  float at;      // start time (s)
  float f0;      // start frequency (Hz)
  float f1;      // end frequency after the glide (Hz); == f0 for a fixed pitch
  float glide;   // glide time (s), 0 = none
  float amp;     // relative level
  float hold;    // sustain before the decay starts (s)
  float tau;     // exponential decay time constant (s)
  Timbre timbre;
};

struct Pattern {
  const Note* notes;
  size_t count;
  float seconds;
};

constexpr float kAttackS = 0.004f;
constexpr float kFadeOutS = 0.02f;

// --- Patterns -------------------------------------------------------------------------

// Watch list: a bright rising C-major arpeggio.
constexpr Note kTracked[] = {
    {0.00f, C5, C5, 0, 0.80f, 0, 0.35f, Timbre::Bell},
    {0.11f, E5, E5, 0, 0.80f, 0, 0.35f, Timbre::Bell},
    {0.22f, G5, G5, 0, 0.80f, 0, 0.35f, Timbre::Bell},
    {0.33f, C6, C6, 0, 0.95f, 0, 0.45f, Timbre::Bell},
};

// Military: a firm rising-fourth horn call, played twice.
constexpr Note kMilitary[] = {
    {0.00f, A4, A4, 0, 0.75f, 0.12f, 0.05f, Timbre::Horn},
    {0.18f, D5, D5, 0, 0.75f, 0.12f, 0.05f, Timbre::Horn},
    {0.42f, A4, A4, 0, 0.75f, 0.12f, 0.05f, Timbre::Horn},
    {0.60f, D5, D5, 0, 0.80f, 0.22f, 0.15f, Timbre::Horn},
};

// Helicopter: quick rotor-like flutter, then a light high note.
constexpr Note kHelicopter[] = {
    {0.00f, E5, E5, 0, 0.70f, 0, 0.025f, Timbre::Soft},
    {0.07f, E5, E5, 0, 0.70f, 0, 0.025f, Timbre::Soft},
    {0.14f, E5, E5, 0, 0.70f, 0, 0.025f, Timbre::Soft},
    {0.21f, E5, E5, 0, 0.70f, 0, 0.025f, Timbre::Soft},
    {0.28f, E5, E5, 0, 0.70f, 0, 0.025f, Timbre::Soft},
    {0.35f, E5, E5, 0, 0.70f, 0, 0.025f, Timbre::Soft},
    {0.48f, B5, B5, 0, 0.85f, 0, 0.30f, Timbre::Bell},
};

// Private / GA: a gentle doorbell "ding-dong".
constexpr Note kPrivate[] = {
    {0.00f, G5, G5, 0, 0.85f, 0, 0.45f, Timbre::Bell},
    {0.30f, E5, E5, 0, 0.85f, 0, 0.55f, Timbre::Bell},
};

// Overhead approach: a soft upward swoosh that lands on a bright bell chord.
constexpr Note kOverhead[] = {
    {0.00f, A4, A5, 0.35f, 0.50f, 0.33f, 0.06f, Timbre::Soft},
    {0.38f, A5, A5, 0, 0.55f, 0, 0.40f, Timbre::Bell},
    {0.38f, E6, E6, 0, 0.60f, 0, 0.40f, Timbre::Bell},
};

// Emergency squawk: urgent alternating hi-lo, three times.
constexpr Note kEmergency[] = {
    {0.00f, A5, A5, 0, 0.85f, 0.12f, 0.04f, Timbre::Horn},
    {0.20f, E5, E5, 0, 0.85f, 0.12f, 0.04f, Timbre::Horn},
    {0.40f, A5, A5, 0, 0.85f, 0.12f, 0.04f, Timbre::Horn},
    {0.60f, E5, E5, 0, 0.85f, 0.12f, 0.04f, Timbre::Horn},
    {0.80f, A5, A5, 0, 0.85f, 0.12f, 0.04f, Timbre::Horn},
    {1.00f, E5, E5, 0, 0.85f, 0.20f, 0.10f, Timbre::Horn},
};

constexpr Note kClick[] = {
    {0.00f, 1800.0f, 1800.0f, 0, 0.6f, 0, 0.006f, Timbre::Soft},
};

// Sound back on: a quick, light two-note rise (shorter than any alert chime).
constexpr Note kConfirm[] = {
    {0.00f, E5, E5, 0, 0.70f, 0, 0.07f, Timbre::Bell},
    {0.08f, A5, A5, 0, 0.80f, 0, 0.12f, Timbre::Bell},
};

#define FS_PATTERN(arr, secs) Pattern{arr, sizeof(arr) / sizeof(arr[0]), secs}

Pattern patternFor(Kind kind) {
  switch (kind) {
    case Kind::Tracked:    return FS_PATTERN(kTracked, 1.20f);
    case Kind::Military:   return FS_PATTERN(kMilitary, 1.10f);
    case Kind::Helicopter: return FS_PATTERN(kHelicopter, 1.10f);
    case Kind::Private:    return FS_PATTERN(kPrivate, 1.30f);
    case Kind::Overhead:   return FS_PATTERN(kOverhead, 1.35f);
    case Kind::Emergency:  return FS_PATTERN(kEmergency, 1.50f);
    case Kind::Click:      return FS_PATTERN(kClick, 0.03f);
    case Kind::Confirm:    return FS_PATTERN(kConfirm, 0.45f);
  }
  return FS_PATTERN(kTracked, 1.20f);
}

#undef FS_PATTERN

/** Partial ratios and levels per timbre; higher partials also decay faster. */
struct Partials {
  float ratio[4];
  float level[4];
  int count;
};

constexpr Partials kBellPartials = {{1.0f, 2.0f, 3.0f, 4.2f}, {1.0f, 0.35f, 0.12f, 0.05f}, 4};
constexpr Partials kHornPartials = {{1.0f, 2.0f, 3.0f, 4.0f}, {1.0f, 0.50f, 0.30f, 0.15f}, 4};
constexpr Partials kSoftPartials = {{1.0f, 2.0f, 0, 0}, {1.0f, 0.12f, 0, 0}, 2};

const Partials& partialsFor(Timbre t) {
  switch (t) {
    case Timbre::Horn: return kHornPartials;
    case Timbre::Soft: return kSoftPartials;
    case Timbre::Bell:
    default:           return kBellPartials;
  }
}

/** Phase (cycles) of a note at time t since its start, with a linear glide f0 -> f1. */
float phaseCycles(const Note& n, float t) {
  if (n.glide <= 0.0f || n.f1 == n.f0) {
    return n.f0 * t;
  }
  if (t < n.glide) {
    return n.f0 * t + (n.f1 - n.f0) * t * t / (2.0f * n.glide);
  }
  return n.f0 * n.glide + (n.f1 - n.f0) * n.glide * 0.5f + n.f1 * (t - n.glide);
}

/** One raw (unnormalized) sample of the whole pattern at time t. */
float sampleAt(const Pattern& p, float t) {
  float acc = 0.0f;
  for (size_t i = 0; i < p.count; ++i) {
    const Note& n = p.notes[i];
    const float nt = t - n.at;
    if (nt < 0.0f) {
      continue;
    }
    float env;
    if (nt < kAttackS) {
      env = nt / kAttackS;
    } else if (nt < n.hold) {
      env = 1.0f;
    } else {
      const float dt = nt - (n.hold > kAttackS ? n.hold : kAttackS);
      if (dt > n.tau * 9.0f) {
        continue;  // below -78 dB: done
      }
      env = expf(-dt / n.tau);
    }
    const Partials& pp = partialsFor(n.timbre);
    const float cyc = phaseCycles(n, nt);
    float v = 0.0f;
    for (int k = 0; k < pp.count; ++k) {
      float lvl = pp.level[k];
      if (k > 0 && n.timbre == Timbre::Bell) {
        // Struck-bell character: overtones die away faster than the fundamental.
        lvl *= expf(-nt * static_cast<float>(k) * 4.0f);
      }
      float c = cyc * pp.ratio[k];
      c -= floorf(c);
      v += lvl * sinf(kTwoPi * c);
    }
    acc += n.amp * env * v;
  }
  return acc;
}

float s_peak_cache[kKindCount] = {};

float peakFor(Kind kind, const Pattern& p, size_t total) {
  const size_t idx = static_cast<size_t>(kind);
  if (idx < kKindCount && s_peak_cache[idx] > 0.0f) {
    return s_peak_cache[idx];
  }
  float peak = 1e-6f;
  for (size_t i = 0; i < total; ++i) {
    const float v = fabsf(sampleAt(p, static_cast<float>(i) / kSampleRate));
    if (v > peak) peak = v;
  }
  if (idx < kKindCount) {
    s_peak_cache[idx] = peak;
  }
  return peak;
}

}  // namespace

size_t length(Kind kind) {
  const Pattern p = patternFor(kind);
  size_t n = static_cast<size_t>(p.seconds * kSampleRate);
  return n > kMaxSamples ? kMaxSamples : n;
}

size_t render(Kind kind, float gain, int16_t* out, size_t max_samples) {
  if (out == nullptr || max_samples == 0) {
    return 0;
  }
  if (gain < 0.0f) gain = 0.0f;
  if (gain > 1.0f) gain = 1.0f;
  const Pattern p = patternFor(kind);
  const size_t full = length(kind);
  const size_t total = full < max_samples ? full : max_samples;
  // Normalize every chime to the same peak so the volume setting means the same thing.
  const float scale = gain * 0.89f * 32767.0f / peakFor(kind, p, full);
  const size_t fade = static_cast<size_t>(kFadeOutS * kSampleRate);
  for (size_t i = 0; i < total; ++i) {
    float v = sampleAt(p, static_cast<float>(i) / kSampleRate) * scale;
    if (i + fade > full) {
      v *= static_cast<float>(full - i) / static_cast<float>(fade);  // no click at the end
    }
    if (v > 32767.0f) v = 32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    out[i] = static_cast<int16_t>(lrintf(v));
  }
  return total;
}

const char* name(Kind kind) {
  switch (kind) {
    case Kind::Tracked:    return "tracked";
    case Kind::Military:   return "military";
    case Kind::Helicopter: return "helicopter";
    case Kind::Private:    return "private";
    case Kind::Overhead:   return "overhead";
    case Kind::Emergency:  return "emergency";
    case Kind::Click:      return "click";
    case Kind::Confirm:    return "confirm";
  }
  return "?";
}

}  // namespace hardware::chime
