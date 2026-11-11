#pragma once

#include <cstdint>

namespace ui {

enum class InfoSettingsPage : uint8_t { Main, Display, Colors, Alerts };

void infoScreenResetToMain();
InfoSettingsPage infoScreenPage();
void infoScreenSetPage(InfoSettingsPage page);

/** Settings / status screens (four pages). */
void infoScreenDraw();

/** Rotate knob on display/colors page adjusts the highlighted row. */
void infoScreenHandleKnob(int8_t delta);

/** Short knob press on display page cycles highlighted setting row. */
void infoScreenCycleDisplayFocus();

/**
 * Touch-only boards: a tap on settings page 2/3. Tapping another row selects it
 * (returns 0); tapping the selected row returns -1 (left half) or +1 (right half),
 * which the caller applies like an encoder step. Always 0 on boards with a knob.
 */
int8_t infoScreenTouch(int16_t x, int16_t y);

/** True when Display page focus is Facing (dial opens orientation adjust). */
bool infoScreenFacingFocused();

/** Default display-page selection when opening page 2. */
void infoScreenResetDisplayFocus();

/** Short knob press on page 3/3 cycles highlighted setting row. */
void infoScreenCycleColorsFocus();

/** Default page-3 selection when opening Colors. */
void infoScreenResetColorsFocus();

/** Page 4/4 "Alerts": type colors and military/emergency/helicopter/private alerts. */
void infoScreenCycleAlertsFocus();
void infoScreenResetAlertsFocus();

}  // namespace ui
