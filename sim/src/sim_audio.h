#pragma once
// Simulator audio (SDL) for the 4C's alert chimes; see sim_audio.cpp.
#include <string>

#include "hardware/chime_synth.h"

namespace sim {

/** Open the PC's default audio output. Call from main() after SDL_Init. */
bool audioOpen();
void audioClose();

/** "military" -> Kind::Military etc. (names from hardware::chime::name). */
bool chimeFromName(const char* name, hardware::chime::Kind* out);

/** Write chime-<name>.wav (full volume) for every chime into dir. Returns 0 on success. */
int dumpChimes(const std::string& dir);

}  // namespace sim
