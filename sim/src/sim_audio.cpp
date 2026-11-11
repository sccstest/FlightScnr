// Simulator speaker: plays the firmware's alert chimes through the PC's audio output via
// an SDL audio queue. Same synthesizer as the 4C's ES8311 path (hardware/chime_synth.h).
#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "hardware/audio_out.h"
#include "sim_audio.h"

namespace sim {
namespace {
std::atomic<SDL_AudioDeviceID> s_dev{0};
}  // namespace

bool audioOpen() {
  if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
    fprintf(stderr, "[sim] no audio: %s\n", SDL_GetError());
    return false;
  }
  SDL_AudioSpec want{};
  want.freq = static_cast<int>(hardware::chime::kSampleRate);
  want.format = AUDIO_S16SYS;
  want.channels = 1;
  want.samples = 512;
  want.callback = nullptr;  // SDL_QueueAudio
  SDL_AudioSpec have{};
  const SDL_AudioDeviceID dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (dev == 0) {
    fprintf(stderr, "[sim] no audio device: %s\n", SDL_GetError());
    return false;
  }
  SDL_PauseAudioDevice(dev, 0);
  s_dev = dev;
  printf("[sim] audio: %d Hz mono via %s\n", have.freq, SDL_GetCurrentAudioDriver());
  return true;
}

void audioClose() {
  const SDL_AudioDeviceID dev = s_dev.exchange(0);
  if (dev != 0) {
    SDL_CloseAudioDevice(dev);
  }
}

bool chimeFromName(const char* name, hardware::chime::Kind* out) {
  for (size_t i = 0; i < hardware::chime::kKindCount; ++i) {
    const auto k = static_cast<hardware::chime::Kind>(i);
    if (strcmp(name, hardware::chime::name(k)) == 0) {
      *out = k;
      return true;
    }
  }
  return false;
}

int dumpChimes(const std::string& dir) {
  std::vector<int16_t> pcm(hardware::chime::kMaxSamples);
  for (size_t i = 0; i < hardware::chime::kKindCount; ++i) {
    const auto k = static_cast<hardware::chime::Kind>(i);
    const size_t n = hardware::chime::render(k, 1.0f, pcm.data(), pcm.size());
    const std::string path = dir + "/chime-" + hardware::chime::name(k) + ".wav";
    FILE* f = fopen(path.c_str(), "wb");
    if (f == nullptr) {
      fprintf(stderr, "[sim] cannot write %s\n", path.c_str());
      return 1;
    }
    const uint32_t rate = hardware::chime::kSampleRate;
    const uint32_t data_bytes = static_cast<uint32_t>(n * 2);
    const uint32_t riff = 36 + data_bytes;
    const uint32_t fmt_len = 16, byte_rate = rate * 2;
    const uint16_t pcm_fmt = 1, ch = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_len, 4, 1, f);
    fwrite(&pcm_fmt, 2, 1, f);
    fwrite(&ch, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_bytes, 4, 1, f);
    fwrite(pcm.data(), 2, n, f);
    fclose(f);
    printf("[sim] wrote %s (%.2f s)\n", path.c_str(), static_cast<double>(n) / rate);
  }
  return 0;
}

}  // namespace sim

namespace hardware::audio_out {

bool init() { return sim::s_dev.load() != 0; }

bool playChime(chime::Kind kind, float gain) {
  const SDL_AudioDeviceID dev = sim::s_dev.load();
  if (dev == 0) {
    return false;
  }
  std::vector<int16_t> pcm(chime::kMaxSamples);
  const size_t n = chime::render(kind, gain, pcm.data(), pcm.size());
  SDL_ClearQueuedAudio(dev);  // a new chime replaces the one playing, like the device
  return SDL_QueueAudio(dev, pcm.data(), static_cast<Uint32>(n * sizeof(int16_t))) == 0;
}

}  // namespace hardware::audio_out
