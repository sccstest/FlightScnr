# FlightScnr desktop simulator (Windows)

Runs the real FlightScnr firmware on a Windows PC, showing the Waveshare
ESP32-P4-WIFI6-Touch-LCD-4C's round 720×720 screen in a window. It's for working on
the UI and data features without the board.

## What's real and what's faked

**Real** (the same source files as the device build):
- `main.cpp`, every screen in `src/ui/`, and the data services: adsb.fi, Open-Meteo / Tomorrow.io weather, the timezone lookup, adsbdb routes, Planespotters photos, airline logos, the route cache and settings
- the P4 screen driver (`src/hardware/p4_dsi_panel.cpp`) and the GT9271 touch driver (`src/hardware/input_p4.cpp`)

**Faked underneath** (`sim/shim/` headers, `sim/src/` code):

| On the device | In the simulator |
| --- | --- |
| MIPI-DSI panel framebuffer in PSRAM | a heap buffer drawn in an SDL window, with a round bezel |
| GT9271 touch on I2C | a fake GT9271 on the fake I2C bus; the mouse is the finger |
| Wi-Fi via the ESP32-C6, TLS via mbedTLS | the PC's network; HTTPS through Windows WinHTTP |
| NVS Preferences | `sim_data/nvs.txt` |
| LittleFS | `sim_data/littlefs/` |
| FreeRTOS tasks, semaphores, esp_timer | `std::thread`, a mutex and condition variable |
| Backlight PWM (GPIO 26) | window brightness |

**Left out:** the Wi-Fi setup portal (`wifi_setup.cpp`), the settings web page
(`settings_web.cpp`) and OTA (`ota_github.cpp`) are stubbed in `sim/src/sim_stubs.cpp`.
Set the radar center with `--lat` / `--lon`. Other settings live in `sim_data/nvs.txt`.

**What it can't tell you:** whether the real panel init, touch chip, ESP-Hosted Wi-Fi,
TLS memory limits or timing work on the hardware. The local clock uses the PC's time
zone. The simulator reports a healthy heap, so the firmware's low-memory recovery paths
never trigger. Those all still need the board.

## Setup (once)

```powershell
winget install BrechtSanders.WinLibs.POSIX.UCRT   # MinGW-w64 GCC
```

The first build downloads SDL2 2.32.10 (the official MinGW dev package) into
`sim/third_party/`, which is git-ignored.

## Run

```powershell
.\sim\run-sim.ps1 -Lat 27.9755 -Lon -82.5332     # build + run, centered on TPA
.\sim\run-sim.ps1 -NoBuild                       # run the last build, last location
```

or `pio run -e sim` and then `.pio\build\sim\program.exe --lat … --lon …`.

- Click = tap. Drag = swipe (the same 140 px threshold as the device).
- **F12** saves a PNG screenshot to `sim_data/`. **Esc** quits.
- `--scale 0.8` makes the window smaller. Touch coordinates still map to the 720×720 panel.

### Scripted runs (UI checks without clicking)

```powershell
.\sim\run-sim.ps1 -NoBuild -Extra '--tap','8:360:677','--shot','20','--tap','34:360:360','--shot','38','--exit-after','40'
```

- `--tap SEC:X:Y` and `--swipe SEC:X1:Y1:X2:Y2` script a finger, in panel pixels.
- `--shot SEC` saves `sim_data/shot-<SEC>s.png`.
- `--exit-after SEC` closes the simulator.
- `--data DIR` uses a different settings/cache folder.

All of these can be repeated. Example: tap the disclaimer's Accept at 8 s, screenshot
the radar at 20 s, tap the middle at 34 s, screenshot at 38 s.

## Licenses

`sim/arduino_core/` holds unmodified copies of WString, Print and Stream from
Arduino-ESP32 3.3.11 (LGPL-2.1, see the file headers). SDL2 (zlib license) is
downloaded at build time and linked statically.
