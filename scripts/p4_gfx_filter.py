"""
P4-only build filter for the vendored Arduino_GFX 1.3.7.

The ESP32-P4 drives its panel through hardware/p4_dsi_panel (MIPI-DSI framebuffer),
so the library's SPI / parallel / RGB data buses are never used — and most of them
poke ESP32/S3 GPIO registers that do not exist on the P4. Drop those translation
units (everything in src/databus/) from the build instead of patching each one.
The base class src/Arduino_DataBus.cpp is kept. S3 environments are unaffected
(this script is only listed in the [p4] section of platformio.ini).

It also drops lib/Arduino_DriveBus/src/hardware/Arduino_HWIIS.cpp. That file uses
IDF's legacy I2S driver (driver/i2s.h), while the P4 speaker code
(audio_out_p4.cpp) uses the new i2s_std driver. IDF 5.x aborts at startup when
both are linked ("CONFLICT! The new i2s driver can't work along with the legacy
i2s driver"), which boot-looped the first flash on real hardware. The P4 build
only uses DriveBus for I2C touch, so nothing references HWIIS.
"""

import os

Import("env")  # type: ignore[name-defined]  # noqa: F821 — PlatformIO SCons


def _skip_gfx_databus(node):
    path = os.path.normcase(node.srcnode().get_abspath())
    parts = path.split(os.sep)
    # Match .../Arduino_GFX-1.3.7/src/databus/<file> exactly (the folder, not the
    # Arduino_DataBus.cpp file, which Windows' case-insensitive globbing would also hit).
    if len(parts) >= 3 and parts[-2] == "databus" and "arduino_gfx-1.3.7" in parts:
        return None
    # Legacy-I2S wrapper in the vendored DriveBus library (see module docstring).
    if parts[-1] == "arduino_hwiis.cpp" and "arduino_drivebus" in parts:
        return None
    return node


env.AddBuildMiddleware(_skip_gfx_databus)  # noqa: F821
