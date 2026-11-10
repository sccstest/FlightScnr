"""
Build setup for the `sim` env (desktop simulator, Windows + MinGW-w64 GCC).

Fetches the official SDL2 MinGW development package from libsdl-org's GitHub
release into sim/third_party/ (git-ignored) on first build, then links SDL2,
WinHTTP and Winsock statically so the result is a single .exe.
"""

import io
import os
import urllib.request
import zipfile
from pathlib import Path

Import("env")  # type: ignore[name-defined]  # noqa: F821 — PlatformIO SCons

SDL_VERSION = "2.32.10"
SDL_URL = (
    "https://github.com/libsdl-org/SDL/releases/download/"
    f"release-{SDL_VERSION}/SDL2-devel-{SDL_VERSION}-mingw.zip"
)

project = Path(env["PROJECT_DIR"])  # noqa: F821
third_party = project / "sim" / "third_party"
sdl_root = third_party / f"SDL2-{SDL_VERSION}" / "x86_64-w64-mingw32"

if not (sdl_root / "lib" / "libSDL2.a").exists():
    print(f"sim: downloading SDL2 {SDL_VERSION} (MinGW dev package) ...")
    third_party.mkdir(parents=True, exist_ok=True)
    with urllib.request.urlopen(SDL_URL, timeout=120) as resp:
        data = resp.read()
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        z.extractall(third_party)
    if not (sdl_root / "lib" / "libSDL2.a").exists():
        raise SystemExit(f"sim: SDL2 not found after extracting {SDL_URL}")
    print(f"sim: SDL2 ready in {sdl_root}")



def _skip_asm(node):
    # JPEGDEC ships ESP32-S3 SIMD assembly; the C fallback is used on the desktop.
    return None if str(node.srcnode()).lower().endswith(".s") else node


env.AddBuildMiddleware(_skip_asm)  # noqa: F821

# Same language levels as the ESP32 toolchain (GCC 16 defaults C to C23, where
# `bool` is a keyword and QRCode's C typedef breaks).
env.Append(CFLAGS=["-std=gnu17"], CXXFLAGS=["-std=gnu++17"])  # noqa: F821

env.Append(  # noqa: F821
    CPPPATH=[str(sdl_root / "include" / "SDL2")],
    LIBPATH=[str(sdl_root / "lib")],
    LINKFLAGS=["-static", "-static-libgcc", "-static-libstdc++"],
    LIBS=[
        "SDL2",
        "winhttp",
        "ws2_32",
        "setupapi",
        "winmm",
        "imm32",
        "version",
        "ole32",
        "oleaut32",
        "gdi32",
        "user32",
        "shell32",
        "advapi32",
        "uuid",
    ],
)
