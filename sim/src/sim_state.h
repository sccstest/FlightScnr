#pragma once
// Shared state between the firmware thread and the SDL window (main thread).
#include <stdint.h>

namespace sim {

/** Framebuffer the fake DPI panel handed to the firmware (RGB565, w*h). */
void setFramebuffer(uint16_t* fb, int w, int h);
uint16_t* framebuffer(int* w, int* h);

/** 0–255 from the LCD_BL PWM; dims the window like the real backlight. */
void setBacklight(uint8_t level);
uint8_t backlight();

/** Mouse-as-finger state, in panel pixels. */
void setTouch(bool down, int x, int y);
bool touch(int* x, int* y);

void requestQuit();
bool quitRequested();
/** Called from delay(): ends the firmware thread once the window is closed. */
void checkQuit();

}  // namespace sim
