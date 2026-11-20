#pragma once

#include <cstddef>
#include <cstdint>

#include "hardware/display_font.h"

namespace ui {
namespace settings_rows {

/**
 * Shared drawing for the option rows on the settings and clock-settings screens.
 *
 * On touch-only boards (Waveshare 4C) every row is a finger-sized band
 * (font height + sc(16), about 70 px at 720 px). The selected row gets a filled
 * pill with a left and a right arrow at its ends: tapping the left half steps the
 * value back, the right half steps it forward. Knob boards keep their compact
 * rows and get the pill without arrows.
 */

struct Colors {
  uint16_t text;        ///< unselected rows
  uint16_t focus_text;  ///< selected row (accent)
  uint16_t focus_fill;  ///< pill behind the selected row
  uint16_t bg;          ///< screen background
};

/** Height of one option row band, gap included. */
int rowPitch(UiTextStyle style);

/** Usable half-width (from the panel center) of a band [top, top + h) on the round face. */
int halfWidthForBand(int top, int h);

/** Shorten text to fit max_width_px, ending in "..." (ASCII; the fonts are 0x20-0x7E). */
void fitText(char* text, size_t len, UiTextStyle style, int max_width_px);

/** Draw one option row in the band [top, top + pitch). */
void drawOptionRow(const char* text, int top, int pitch, UiTextStyle style, bool focused,
                   const Colors& colors);

/** Common settings colors. */
Colors defaultColors();

}  // namespace settings_rows
}  // namespace ui
