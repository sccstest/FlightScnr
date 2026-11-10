"""
P4-only build filter for the vendored Arduino_GFX 1.3.7.

The ESP32-P4 drives its panel through hardware/p4_dsi_panel (MIPI-DSI framebuffer),
so the library's SPI / parallel / RGB data buses are never used — and most of them
poke ESP32/S3 GPIO registers that do not exist on the P4. Drop those translation
units (everything in src/databus/) from the build instead of patching each one.
The base class src/Arduino_DataBus.cpp is kept. S3 environments are unaffected
(this script is only listed in the [p4] section of platformio.ini).
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
    return node


env.AddBuildMiddleware(_skip_gfx_databus)  # noqa: F821
