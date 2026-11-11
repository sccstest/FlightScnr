#pragma once

/**
 * Waveshare ESP32-P4-WIFI6-Touch-LCD-4C — 4" round 720×720 IPS, 2-lane MIPI-DSI.
 * https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-3.4c.htm (select "4C")
 * Reference: github.com/waveshareteam/ESP32-P4-WIFI6-Touch-LCD-XC (Apache-2.0),
 *   examples/arduino/libraries/displays/displays_config.h (SCREEN_4INCH_DSI).
 *
 * - ESP32-P4NRW32: 32 MB PSRAM, 32 MB flash. Wi-Fi via ESP32-C6 over SDIO (ESP-Hosted).
 * - Panel: JD9365-class DSI controller, 1500 Mbps/lane, 80 MHz DPI clock, RGB565 framebuffer.
 * - Touch: GT9271 (GT911-compatible register map) on I2C, polled (INT/RST not wired to driver).
 * - Audio: ES8311 codec + PA, 8 Ω speaker on PH2.0 header: alert chimes (no piezo buzzer).
 * - No rotary encoder or knob button: all navigation is touch.
 */

/* Rev3 and pre-rev3 P4 binaries are not interchangeable, so each env has its
 * own board name: OTA fetches FlightScnr-<name>-app.bin and the WebFlasher
 * board marker uses it too. platformio.ini sets FLIGHTSCNR_P4_PRE_REV3 for
 * the waveshare-p4-4c-v1 env. */
#if defined(FLIGHTSCNR_P4_PRE_REV3) && FLIGHTSCNR_P4_PRE_REV3
#define FLIGHTSCNR_BOARD_NAME "waveshare-p4-4c-v1"
#else
#define FLIGHTSCNR_BOARD_NAME "waveshare-p4-4c"
#endif
#define FLIGHTSCNR_BOARD_WAVESHARE_P4_4C 1

/* The P4 port lives in a fork; upstream releases have no P4 image. */
#define FLIGHTSCNR_OTA_REPO "sccstest/FlightScnr"

#define FLIGHTSCNR_HAS_KNOB_BUTTON 0
#define FLIGHTSCNR_HAS_ENCODER 0
#define FLIGHTSCNR_HAS_BUZZER 0
#define FLIGHTSCNR_HAS_HAPTIC 0
/* ES8311 + speaker: alert chimes (src/hardware/audio_out_p4.cpp). */
#define FLIGHTSCNR_HAS_SPEAKER 1
#define FLIGHTSCNR_HAS_LCD_VCI_EN 0
#define FLIGHTSCNR_HAS_LCD_BACKLIGHT_PWM 1
#define FLIGHTSCNR_PANEL_AUTODETECT 0

/* Shared I2C bus (touch, ES8311, RTC). */
#define IIC_SDA 7
#define IIC_SCL 8
#define GT911_SLAVE_ADDRESS 0x5D
#define GT911_SLAVE_ADDRESS_ALT 0x14

/* Polling driver: Waveshare's examples do not drive touch INT/RST. */
#define TOUCH_INT -1
#define TOUCH_RST -1

#define LCD_RST 27
#define LCD_BL 26
#define LCD_WIDTH 720
#define LCD_HEIGHT 720

/* MIPI-DSI timing for the 4C panel (Waveshare displays_config.h, SCREEN_4INCH_DSI). */
#define LCD_DSI_LANES 2
#define LCD_DSI_LANE_MBPS 1500
#define LCD_DPI_CLOCK_MHZ 80
#define LCD_HSYNC_PULSE 20
#define LCD_HSYNC_BACK_PORCH 20
#define LCD_HSYNC_FRONT_PORCH 40
#define LCD_VSYNC_PULSE 4
#define LCD_VSYNC_BACK_PORCH 12
#define LCD_VSYNC_FRONT_PORCH 24
/* LDO_VO3 feeds VDD_MIPI_DPHY. */
#define LCD_DSI_PHY_LDO_CHAN 3
#define LCD_DSI_PHY_LDO_MV 2500

/* No rotary encoder; config.h still names the pins. GPIO_NUM_NC = -1. */
#define KNOB_DATA_A -1
#define KNOB_DATA_B -1
#define KNOB_KEY -1

/** Only rotation 0 is implemented by the framebuffer driver so far. */
#define FLIGHTSCNR_DISPLAY_ROTATION 0

/* Speaker path: ES8311 codec (I2C 0x18 on the shared bus) + I2S + PA. */
#define ES8311_I2C_ADDRESS 0x18
#define AUDIO_PA_EN 53
#define AUDIO_I2S_MCLK 13
#define AUDIO_I2S_BCLK 12
#define AUDIO_I2S_LRCK 10
#define AUDIO_I2S_DOUT 9
