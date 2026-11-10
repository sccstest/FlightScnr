#pragma once

#include "hardware/pin_config.h"

namespace ui {

/**
 * The screens were laid out in pixels for the 390x390 T-Encoder Pro (the Knob 1.8
 * at 360 px reuses the same numbers). sc() maps those design pixels to the
 * actual panel so a layout keeps its proportions on a larger round screen.
 *
 * On the S3 boards it returns its argument unchanged, so their layouts are
 * byte-for-byte what they were. On the Waveshare 4C (720 px) it scales by 720/390.
 */
constexpr int kUiDesignPx = 390;

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
constexpr int sc(int design_px) {
  return design_px >= 0 ? (design_px * LCD_WIDTH + kUiDesignPx / 2) / kUiDesignPx
                        : -((-design_px * LCD_WIDTH + kUiDesignPx / 2) / kUiDesignPx);
}
constexpr float scf(float design_px) {
  return design_px * static_cast<float>(LCD_WIDTH) / static_cast<float>(kUiDesignPx);
}
#else
constexpr int sc(int design_px) { return design_px; }
constexpr float scf(float design_px) { return design_px; }
#endif

}  // namespace ui
