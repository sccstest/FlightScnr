#pragma once

#include <cstdint>

namespace ui {

/**
 * Settings pages, in swipe order. Main is the "Status" page. The other names are
 * kept from the old four-page layout (main.cpp keys tap handling off them); what
 * each page holds is listed in info_screen.cpp. The 4C adds two pages so every
 * row is a finger-sized target.
 */
enum class InfoSettingsPage : uint8_t {
  Main,
  Display,
  Colors,  ///< "Radar" on the 4C, "Radar & sound" on the knob boards
  Alerts,
#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)
  Sound,   ///< "Sound & sky"
  Timers,  ///< "Screen timers"
#endif
};

void infoScreenResetToMain();
InfoSettingsPage infoScreenPage();
void infoScreenSetPage(InfoSettingsPage page);

/** Swipe left / right between pages. False at the last / first page (no change). */
bool infoScreenNextPage();
bool infoScreenPrevPage();
/** True on the last settings page. */
bool infoScreenOnLastPage();
/** 1-based number of the current page, and the page count (for logs). */
int infoScreenPageNumber();
int infoScreenPageCount();

/** Settings / status screens. */
void infoScreenDraw();

/** Knob turn or side tap: step the selected row's value. */
void infoScreenHandleKnob(int8_t delta);

/** Short knob press: select the next row on the current page. */
void infoScreenCycleDisplayFocus();

/**
 * Touch-only boards: a tap on an option page. Tapping another row selects it
 * (returns 0); tapping the selected row returns -1 (left half) or +1 (right half),
 * which the caller applies like an encoder step. Always 0 on boards with a knob.
 */
int8_t infoScreenTouch(int16_t x, int16_t y);

/** Result of a tap that main.cpp routes through infoScreenMainTap(). */
enum class MainPageTap : uint8_t {
  None,            ///< Not on the Reset Wi-Fi row.
  ResetArmed,      ///< First tap: row now reads "Tap again to reset" (redraw).
  ResetConfirmed,  ///< Second tap within 5 s: caller wipes Wi-Fi and reboots.
  Cancelled,       ///< Tapped elsewhere while armed (redraw).
  Handled,         ///< A tap on an option page main.cpp has no branch for (redraw).
};

/**
 * Touch tap on settings page 1. On boards without a knob button (Waveshare 4C and
 * Knob 1.8) the page has a "Reset Wi-Fi" button with a tap-again confirm, the touch
 * stand-in for the 5-second knob hold. Always None on boards with a knob button.
 *
 * main.cpp also sends taps here for the 4C's extra pages (Sound, Timers); those are
 * handled like infoScreenTouch() + infoScreenHandleKnob() and return Handled.
 */
MainPageTap infoScreenMainTap(int16_t x, int16_t y);

/** True once when an armed Wi-Fi reset expires unconfirmed (caller redraws). */
bool infoScreenWifiResetTimedOut();

/** True when the selected row is Facing (dial / side tap opens orientation adjust). */
bool infoScreenFacingFocused();

/** Select the first row (called when a page opens). */
void infoScreenResetDisplayFocus();

/** Short knob press on the Colors page: select the next row. */
void infoScreenCycleColorsFocus();

/** Select the first row (called when a page opens). */
void infoScreenResetColorsFocus();

/** Short knob press on the Alerts page: select the next row. */
void infoScreenCycleAlertsFocus();
void infoScreenResetAlertsFocus();

}  // namespace ui
