// FlightScnr desktop simulator: runs the firmware's setup()/loop() on a worker
// thread and shows the 720x720 round panel in an SDL window. Mouse = finger.
//
//   program.exe [--lat 27.95 --lon -82.46] [--scale 0.8] [--data DIR]
//               [--tap SEC:X:Y]... [--swipe SEC:X1:Y1:X2:Y2]... [--shot SEC]...
//               [--exit-after SEC]
//
// --tap / --swipe script a finger at a time after start (panel pixels, 0-719);
// --shot saves sim_data/shot-<SEC>s.png. All three can be repeated.
//
// Keys: F12 = save PNG screenshot to sim_data/, Esc = quit.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <Arduino.h>
#include <Preferences.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hardware/pin_config.h"
#include "sim_state.h"

void setup();
void loop();

namespace sim {
std::string dataDir();

namespace {
std::atomic<uint16_t*> s_fb{nullptr};
std::atomic<int> s_fb_w{0};
std::atomic<int> s_fb_h{0};
std::atomic<uint8_t> s_backlight{255};
std::atomic<bool> s_touch_down{false};
std::atomic<int> s_touch_x{0};
std::atomic<int> s_touch_y{0};
std::atomic<bool> s_quit{false};
}  // namespace

void setFramebuffer(uint16_t* fb, int w, int h) {
  s_fb_w = w;
  s_fb_h = h;
  s_fb = fb;
}
uint16_t* framebuffer(int* w, int* h) {
  *w = s_fb_w;
  *h = s_fb_h;
  return s_fb;
}
void setBacklight(uint8_t level) { s_backlight = level; }
uint8_t backlight() { return s_backlight; }
void setTouch(bool down, int x, int y) {
  s_touch_x = x;
  s_touch_y = y;
  s_touch_down = down;
}
bool touch(int* x, int* y) {
  *x = s_touch_x;
  *y = s_touch_y;
  return s_touch_down;
}
void requestQuit() { s_quit = true; }
bool quitRequested() { return s_quit; }
void checkQuit() {}

}  // namespace sim

namespace {

// --- Minimal PNG writer (stored deflate blocks), so screenshots open anywhere.
uint32_t crc32(const uint8_t* d, size_t n, uint32_t c = 0xFFFFFFFFu) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t v = i;
      for (int k = 0; k < 8; ++k) v = (v & 1) ? 0xEDB88320u ^ (v >> 1) : v >> 1;
      table[i] = v;
    }
    init = true;
  }
  for (size_t i = 0; i < n; ++i) c = table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
  return c;
}

void put32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(x >> 24);
  v.push_back(x >> 16);
  v.push_back(x >> 8);
  v.push_back(x);
}

void chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& data) {
  put32(out, static_cast<uint32_t>(data.size()));
  std::vector<uint8_t> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  out.insert(out.end(), td.begin(), td.end());
  put32(out, crc32(td.data(), td.size()) ^ 0xFFFFFFFFu);
}

bool savePng(const std::string& path, const uint16_t* fb, int w, int h) {
  std::vector<uint8_t> raw;
  raw.reserve(static_cast<size_t>(h) * (1 + w * 3));
  const float cx = (w - 1) / 2.0f;
  const float cy = (h - 1) / 2.0f;
  const float r2 = (w / 2.0f) * (w / 2.0f);
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);
    for (int x = 0; x < w; ++x) {
      const uint16_t p = fb[y * w + x];
      const bool inside = (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r2;
      raw.push_back(inside ? static_cast<uint8_t>(((p >> 11) & 0x1F) * 255 / 31) : 40);
      raw.push_back(inside ? static_cast<uint8_t>(((p >> 5) & 0x3F) * 255 / 63) : 40);
      raw.push_back(inside ? static_cast<uint8_t>((p & 0x1F) * 255 / 31) : 40);
    }
  }
  std::vector<uint8_t> z = {0x78, 0x01};
  uint32_t a = 1, b = 0;
  for (uint8_t c : raw) {
    a = (a + c) % 65521;
    b = (b + a) % 65521;
  }
  for (size_t off = 0; off < raw.size(); off += 65535) {
    const size_t n = std::min<size_t>(65535, raw.size() - off);
    z.push_back(off + n >= raw.size() ? 1 : 0);
    z.push_back(n & 0xFF);
    z.push_back(n >> 8);
    z.push_back(~n & 0xFF);
    z.push_back((~n >> 8) & 0xFF);
    z.insert(z.end(), raw.begin() + off, raw.begin() + off + n);
  }
  put32(z, (b << 16) | a);

  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
  std::vector<uint8_t> ihdr;
  put32(ihdr, w);
  put32(ihdr, h);
  ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", {});
  FILE* f = fopen(path.c_str(), "wb");
  if (f == nullptr) return false;
  fwrite(png.data(), 1, png.size(), f);
  fclose(f);
  return true;
}

std::string screenshot(const std::string& name = "") {
  int w = 0, h = 0;
  uint16_t* fb = sim::framebuffer(&w, &h);
  if (fb == nullptr) return "";
  std::filesystem::create_directories(sim::dataDir());
  std::string path = sim::dataDir() + "/";
  if (name.empty()) {
    char stamp[64];
    const time_t now = time(nullptr);
    struct tm t;
    localtime_s(&t, &now);
    strftime(stamp, sizeof(stamp), "screenshot-%Y%m%d-%H%M%S.png", &t);
    path += stamp;
  } else {
    path += name;
  }
  std::vector<uint16_t> copy(fb, fb + static_cast<size_t>(w) * h);
  if (!savePng(path, copy.data(), w, h)) return "";
  printf("[sim] screenshot saved: %s\n", path.c_str());
  fflush(stdout);
  return path;
}

double argDouble(int argc, char** argv, const char* name, double def) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (strcmp(argv[i], name) == 0) return atof(argv[i + 1]);
  }
  return def;
}

struct ScriptEvent {
  double at;
  enum Kind { Down, Move, Up, Shot } kind;
  int x;
  int y;
};

std::vector<ScriptEvent> parseScript(int argc, char** argv) {
  std::vector<ScriptEvent> ev;
  for (int i = 1; i + 1 < argc; ++i) {
    const char* v = argv[i + 1];
    double t = 0;
    int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    if (strcmp(argv[i], "--tap") == 0 && sscanf(v, "%lf:%d:%d", &t, &x1, &y1) == 3) {
      ev.push_back({t, ScriptEvent::Down, x1, y1});
      ev.push_back({t + 0.12, ScriptEvent::Up, x1, y1});
    } else if (strcmp(argv[i], "--swipe") == 0 &&
               sscanf(v, "%lf:%d:%d:%d:%d", &t, &x1, &y1, &x2, &y2) == 5) {
      ev.push_back({t, ScriptEvent::Down, x1, y1});
      for (int k = 1; k <= 8; ++k) {
        ev.push_back({t + 0.03 * k, ScriptEvent::Move, x1 + (x2 - x1) * k / 8,
                      y1 + (y2 - y1) * k / 8});
      }
      ev.push_back({t + 0.3, ScriptEvent::Up, x2, y2});
    } else if (strcmp(argv[i], "--shot") == 0 || strcmp(argv[i], "--screenshot-after") == 0) {
      ev.push_back({atof(v), ScriptEvent::Shot, 0, 0});
    }
  }
  return ev;
}

const char* argStr(int argc, char** argv, const char* name) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (strcmp(argv[i], name) == 0) return argv[i + 1];
  }
  return nullptr;
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  if (const char* dir = argStr(argc, argv, "--data")) {
    std::string env = std::string("FLIGHTSCNR_SIM_DATA=") + dir;
    _putenv(env.c_str());
  }
  const double lat = argDouble(argc, argv, "--lat", NAN);
  const double lon = argDouble(argc, argv, "--lon", NAN);
  if (!std::isnan(lat) && !std::isnan(lon)) {
    Preferences prefs;
    prefs.begin("fs_geo", false);
    prefs.putDouble("center_lat", lat);
    prefs.putDouble("center_lon", lon);
    prefs.end();
    printf("[sim] radar center set to %.5f, %.5f\n", lat, lon);
  }
  const double scale = argDouble(argc, argv, "--scale", 1.0);
  std::vector<ScriptEvent> script = parseScript(argc, argv);
  const double exit_after = argDouble(argc, argv, "--exit-after", -1);

  SDL_SetMainReady();
  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
    return 1;
  }
  const int panel = LCD_WIDTH;
  const int win = static_cast<int>(panel * scale);
  SDL_Window* window =
      SDL_CreateWindow("FlightScnr simulator — Waveshare 4C (720x720)", SDL_WINDOWPOS_CENTERED,
                       SDL_WINDOWPOS_CENTERED, win, win, SDL_WINDOW_SHOWN);
  SDL_Renderer* ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
  if (ren == nullptr) ren = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
  SDL_RenderSetLogicalSize(ren, panel, panel);
  SDL_Texture* tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                       panel, panel);

  // Round bezel: opaque dark outside the 720 px circle, transparent inside.
  SDL_Texture* bezel = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                         SDL_TEXTUREACCESS_STATIC, panel, panel);
  {
    std::vector<uint32_t> px(static_cast<size_t>(panel) * panel);
    const float c = (panel - 1) / 2.0f;
    const float r = panel / 2.0f;
    for (int y = 0; y < panel; ++y) {
      for (int x = 0; x < panel; ++x) {
        const float d = std::sqrt((x - c) * (x - c) + (y - c) * (y - c));
        const float edge = std::min(1.0f, std::max(0.0f, d - (r - 1.0f)));
        const uint8_t alpha = static_cast<uint8_t>(edge * 255.0f);
        px[static_cast<size_t>(y) * panel + x] = (static_cast<uint32_t>(alpha) << 24) | 0x202020;
      }
    }
    SDL_UpdateTexture(bezel, nullptr, px.data(), panel * 4);
    SDL_SetTextureBlendMode(bezel, SDL_BLENDMODE_BLEND);
  }

  std::thread firmware([] {
    setup();
    while (!sim::quitRequested()) {
      loop();
    }
  });
  firmware.detach();

  const auto start = std::chrono::steady_clock::now();
  size_t next_event = 0;
  std::sort(script.begin(), script.end(),
            [](const ScriptEvent& a, const ScriptEvent& b) { return a.at < b.at; });
  bool running = true;
  while (running) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      switch (e.type) {
        case SDL_QUIT:
          running = false;
          break;
        case SDL_KEYDOWN:
          if (e.key.keysym.sym == SDLK_ESCAPE) running = false;
          if (e.key.keysym.sym == SDLK_F12) screenshot();
          break;
        case SDL_MOUSEBUTTONDOWN:
          if (e.button.button == SDL_BUTTON_LEFT) sim::setTouch(true, e.button.x, e.button.y);
          break;
        case SDL_MOUSEBUTTONUP:
          if (e.button.button == SDL_BUTTON_LEFT) sim::setTouch(false, e.button.x, e.button.y);
          break;
        case SDL_MOUSEMOTION:
          if (e.motion.state & SDL_BUTTON_LMASK) sim::setTouch(true, e.motion.x, e.motion.y);
          break;
        default:
          break;
      }
    }

    int w = 0, h = 0;
    uint16_t* fb = sim::framebuffer(&w, &h);
    SDL_SetRenderDrawColor(ren, 0x20, 0x20, 0x20, 255);
    SDL_RenderClear(ren);
    if (fb != nullptr && w == panel && h == panel) {
      SDL_UpdateTexture(tex, nullptr, fb, w * 2);
      const uint8_t bl = sim::backlight();
      SDL_SetTextureColorMod(tex, bl, bl, bl);
      SDL_RenderCopy(ren, tex, nullptr, nullptr);
    }
    SDL_RenderCopy(ren, bezel, nullptr, nullptr);
    SDL_RenderPresent(ren);

    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    while (next_event < script.size() && secs >= script[next_event].at) {
      const ScriptEvent& ev = script[next_event++];
      if (ev.kind == ScriptEvent::Shot) {
        char name[48];
        snprintf(name, sizeof(name), "shot-%gs.png", ev.at);
        screenshot(name);
      } else {
        sim::setTouch(ev.kind != ScriptEvent::Up, ev.x, ev.y);
      }
    }
    if (exit_after >= 0 && secs >= exit_after) running = false;
    SDL_Delay(16);
  }

  sim::requestQuit();
  SDL_DestroyWindow(window);
  SDL_Quit();
  fflush(stdout);
  _exit(0);  // firmware/worker threads are detached and may be mid-request
}
