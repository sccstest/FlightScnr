#pragma once

/**
 * ESP32-P4 MIPI-DSI panel driver for the vendored Arduino_GFX 1.3.7.
 *
 * The rest of FlightScnr talks to the panel as an Arduino_TFT (address window +
 * writePixels / writeRepeat). On the P4 the panel is a DPI framebuffer in PSRAM
 * that the DSI peripheral scans out continuously, so this driver:
 *   - P4FramebufferBus: an Arduino_DataBus whose "pixel stream" lands in the
 *     current address window of the framebuffer, and which writes dirty rows
 *     back from cache on endWrite() so the DSI DMA sees them.
 *   - Arduino_P4Dsi: an Arduino_TFT that brings up the DSI bus, sends the
 *     vendor init table and maps writeAddrWindow() onto the bus window.
 *
 * Only rotation 0 is supported for now.
 */

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)

#include <Arduino_GFX_Library.h>
#include <cstddef>
#include <cstdint>

struct P4DsiInitCmd {
  uint8_t cmd;
  const uint8_t* data;
  uint8_t data_bytes;
  uint16_t delay_ms;
};

struct P4DsiConfig {
  uint16_t width;
  uint16_t height;
  uint8_t lanes;
  uint32_t lane_mbps;
  uint32_t dpi_clock_mhz;
  uint32_t hsync_pulse;
  uint32_t hsync_back_porch;
  uint32_t hsync_front_porch;
  uint32_t vsync_pulse;
  uint32_t vsync_back_porch;
  uint32_t vsync_front_porch;
  int phy_ldo_chan;
  int phy_ldo_mv;
  const P4DsiInitCmd* init_cmds;
  size_t init_cmds_len;
};

class P4FramebufferBus : public Arduino_DataBus {
 public:
  P4FramebufferBus() = default;

  void attachFramebuffer(uint16_t* fb, uint16_t width, uint16_t height);
  void setWindow(int16_t x, int16_t y, uint16_t w, uint16_t h);
  /** Write back cached framebuffer rows touched since the last flush. */
  void flushDirty();

  bool begin(int32_t speed = GFX_NOT_DEFINED, int8_t dataMode = GFX_NOT_DEFINED) override;
  void beginWrite() override;
  void endWrite() override;
  void writeCommand(uint8_t c) override;
  void writeCommand16(uint16_t c) override;
  void write(uint8_t d) override;
  void write16(uint16_t d) override;
  void writeRepeat(uint16_t p, uint32_t len) override;
  void writePixels(uint16_t* data, uint32_t len) override;
  void writeC8D8(uint8_t c, uint8_t d) override;
  void writeC16D16(uint16_t c, uint16_t d) override;
  void writeC8D16(uint8_t c, uint16_t d) override;
  void writeC8D16D16(uint8_t c, uint16_t d1, uint16_t d2) override;
  void writeC8D16D16Split(uint8_t c, uint16_t d1, uint16_t d2) override;
  void batchOperation(const uint8_t* operations, size_t len) override;
  void writeBytes(uint8_t* data, uint32_t len) override;

 private:
  inline void putPixel(uint16_t color);
  void markDirty(int16_t y0, int16_t y1);

  uint16_t* fb_ = nullptr;
  uint16_t fb_w_ = 0;
  uint16_t fb_h_ = 0;

  int16_t win_x0_ = 0;
  int16_t win_y0_ = 0;
  int16_t win_x1_ = 0;  // inclusive
  int16_t win_y1_ = 0;  // inclusive
  int16_t cur_x_ = 0;
  int16_t cur_y_ = 0;

  int16_t dirty_y0_ = INT16_MAX;
  int16_t dirty_y1_ = -1;
  uint8_t write_depth_ = 0;

  bool have_msb_ = false;
  uint8_t msb_ = 0;
};

class Arduino_P4Dsi : public Arduino_TFT {
 public:
  Arduino_P4Dsi(P4FramebufferBus* bus, int8_t rst, const P4DsiConfig& cfg);

  void writeAddrWindow(int16_t x, int16_t y, uint16_t w, uint16_t h) override;
  void setRotation(uint8_t r) override;
  void invertDisplay(bool i) override;
  void displayOn() override;
  void displayOff() override;
  /** Brightness is the LCD_BL PWM (display_brightness.cpp), not a DCS command. */
  void Display_Brightness(uint8_t brightness) override;

  uint16_t* framebuffer() const { return fb_; }

 protected:
  void tftInit() override;

 private:
  void sendDcs(uint8_t cmd, const uint8_t* data, size_t len);

  P4FramebufferBus* fb_bus_;
  P4DsiConfig cfg_;
  uint16_t* fb_ = nullptr;
  void* dbi_io_ = nullptr;     // esp_lcd_panel_io_handle_t
  void* dpi_panel_ = nullptr;  // esp_lcd_panel_handle_t
};

#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C
