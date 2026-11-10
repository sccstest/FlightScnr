#include "hardware/display_font.h"

#include <algorithm>
#include <cstdlib>

#include "fonts/MontserratBold10pt4aa.h"
#include "fonts/MontserratBold11pt4aa.h"
#include "fonts/MontserratBold12pt4aa.h"
#include "fonts/MontserratBold14pt4aa.h"
#include "fonts/MontserratBold18pt4aa.h"
#include "fonts/MontserratBold24pt4aa.h"
#include "fonts/MontserratBold36pt4aa.h"
#include "fonts/MontserratBold8pt4aa.h"
#include "fonts/MontserratBold9pt4aa.h"
#include "hardware/pin_config.h"

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
// 720 px panel: every preset is the 390 px design size x 720/390 (see ui/ui_scale.h).
#include "fonts/MontserratBold15pt4aa.h"
#include "fonts/MontserratBold17pt4aa.h"
#include "fonts/MontserratBold20pt4aa.h"
#include "fonts/MontserratBold22pt4aa.h"
#include "fonts/MontserratBold26pt4aa.h"
#include "fonts/MontserratBold33pt4aa.h"
#include "fonts/MontserratBold44pt4aa.h"
#include "fonts/MontserratBold66pt4aa.h"
#define FS_FONT_8 MontserratBold15pt4aa
#define FS_FONT_9 MontserratBold17pt4aa
#define FS_FONT_10 MontserratBold18pt4aa
#define FS_FONT_11 MontserratBold20pt4aa
#define FS_FONT_12 MontserratBold22pt4aa
#define FS_FONT_14 MontserratBold26pt4aa
#define FS_FONT_18 MontserratBold33pt4aa
#define FS_FONT_24 MontserratBold44pt4aa
#define FS_FONT_36 MontserratBold66pt4aa
#else
#define FS_FONT_8 MontserratBold8pt4aa
#define FS_FONT_9 MontserratBold9pt4aa
#define FS_FONT_10 MontserratBold10pt4aa
#define FS_FONT_11 MontserratBold11pt4aa
#define FS_FONT_12 MontserratBold12pt4aa
#define FS_FONT_14 MontserratBold14pt4aa
#define FS_FONT_18 MontserratBold18pt4aa
#define FS_FONT_24 MontserratBold24pt4aa
#define FS_FONT_36 MontserratBold36pt4aa
#endif

namespace {

const AaFont* kFonts[] = {
    &FS_FONT_8,
    &FS_FONT_9,
    &FS_FONT_10,
    &FS_FONT_11,
    &FS_FONT_12,
    &FS_FONT_14,
    &FS_FONT_18,
};

constexpr size_t kFontCount = sizeof(kFonts) / sizeof(kFonts[0]);

int absDiff(int a, int b) { return std::abs(a - b); }

}  // namespace

UiTextStyle displayFontTitle() { return UiTextStyle{&FS_FONT_18}; }

UiTextStyle displayFontBody() { return UiTextStyle{&FS_FONT_12}; }

UiTextStyle displayFontDetail() { return UiTextStyle{&FS_FONT_10}; }

UiTextStyle displayFontCardinal() { return UiTextStyle{&FS_FONT_12}; }

UiTextStyle displayFontScale() { return UiTextStyle{&FS_FONT_9}; }

UiTextStyle displayFontTag() { return UiTextStyle{&FS_FONT_11}; }

UiTextStyle displayFontClockTime() { return UiTextStyle{&FS_FONT_36}; }

UiTextStyle displayFontClockAmPm() { return UiTextStyle{&FS_FONT_24}; }

UiTextStyle displayFontClockDate() { return UiTextStyle{&FS_FONT_18}; }

UiTextStyle displayFontPickForHeight(PlaneGfx& gfx, int target_px, size_t lo_index,
                                     size_t hi_index) {
  if (kFontCount == 0) {
    return displayFontBody();
  }
  lo_index = std::min(lo_index, kFontCount - 1);
  hi_index = std::min(hi_index, kFontCount - 1);
  if (hi_index < lo_index) {
    std::swap(lo_index, hi_index);
  }

  size_t best = lo_index;
  int best_diff = absDiff(displayFontHeight(gfx, UiTextStyle{kFonts[best]}), target_px);
  for (size_t i = lo_index; i <= hi_index; ++i) {
    const int diff =
        absDiff(displayFontHeight(gfx, UiTextStyle{kFonts[i]}), target_px);
    if (diff < best_diff) {
      best_diff = diff;
      best = i;
    }
  }
  return UiTextStyle{kFonts[best]};
}

void displayFontApply(PlaneGfx& gfx, UiTextStyle style) {
  gfx.setTextWrap(false);
  gfx.setTextSize(1);
  gfx.setFont(style.font);
}

int displayFontHeight(PlaneGfx& gfx, UiTextStyle style) {
  displayFontApply(gfx, style);
  return gfx.fontHeight();
}

int displayFontWidth(PlaneGfx& gfx, UiTextStyle style, const char* text) {
  displayFontApply(gfx, style);
  return gfx.textWidth(text);
}
