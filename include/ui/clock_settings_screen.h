#pragma once

#include <cstdint>

namespace ui {

void clockSettingsScreenDraw();
void clockSettingsHandleKnob(int8_t delta);
void clockSettingsCycleFocus();
/** Touch-only boards: tap selects a row (returns 0) or steps the selected one (-1/+1). */
int8_t clockSettingsTouch(int16_t x, int16_t y);
void clockSettingsResetFocus();

}  // namespace ui
