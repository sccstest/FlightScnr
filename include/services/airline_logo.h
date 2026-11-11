#pragma once

#include <cstdint>

class PlaneGfx;

namespace services::airline {

/** Draw airline logo for ICAO operator code (3 letters); returns true if drawn. */
bool drawLogo(PlaneGfx& tft, const char* icao, int16_t center_x, int16_t y, uint16_t bg);

/** Logo height in pixels for layout (0 when unknown / missing). */
int logoHeightForIcao(const char* icao);

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
/** Decode a logo to RGB565 (transparent pixels = kTransparentRgb565). The pixels live in
 *  the shared decode buffer: copy them before the next drawLogo/decode/release. */
bool decodeLogoPixels(const char* icao, const uint16_t** pixels, int* w, int* h);
#endif

/** Free decode buffer (~15KB) when leaving flight detail. */
void releaseLogoBuffer();

}  // namespace services::airline
