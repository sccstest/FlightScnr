#include "hardware/p4_dsi_panel.h"

#if defined(FLIGHTSCNR_BOARD_WAVESHARE_P4_4C)

#include <Arduino.h>
#include <esp_cache.h>
#include <esp_err.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_ldo_regulator.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// P4FramebufferBus
// ---------------------------------------------------------------------------

void P4FramebufferBus::attachFramebuffer(uint16_t* fb, uint16_t width, uint16_t height) {
  fb_ = fb;
  fb_w_ = width;
  fb_h_ = height;
  setWindow(0, 0, width, height);
}

void P4FramebufferBus::setWindow(int16_t x, int16_t y, uint16_t w, uint16_t h) {
  win_x0_ = x;
  win_y0_ = y;
  win_x1_ = static_cast<int16_t>(x + static_cast<int16_t>(w) - 1);
  win_y1_ = static_cast<int16_t>(y + static_cast<int16_t>(h) - 1);
  cur_x_ = x;
  cur_y_ = y;
  have_msb_ = false;
}

void P4FramebufferBus::markDirty(int16_t y0, int16_t y1) {
  if (y0 < dirty_y0_) dirty_y0_ = y0;
  if (y1 > dirty_y1_) dirty_y1_ = y1;
}

void P4FramebufferBus::flushDirty() {
  if (fb_ == nullptr || dirty_y1_ < 0) {
    return;
  }
  const int16_t y0 = std::max<int16_t>(0, dirty_y0_);
  const int16_t y1 = std::min<int16_t>(static_cast<int16_t>(fb_h_ - 1), dirty_y1_);
  dirty_y0_ = INT16_MAX;
  dirty_y1_ = -1;
  if (y1 < y0) {
    return;
  }
  uint16_t* start = fb_ + static_cast<size_t>(y0) * fb_w_;
  const size_t bytes = static_cast<size_t>(y1 - y0 + 1) * fb_w_ * sizeof(uint16_t);
  esp_cache_msync(start, bytes,
                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}

inline void P4FramebufferBus::putPixel(uint16_t color) {
  if (cur_y_ > win_y1_) {
    return;  // stream overran the window; drop like a real panel would
  }
  if (cur_x_ >= 0 && cur_x_ < fb_w_ && cur_y_ >= 0 && cur_y_ < fb_h_) {
    fb_[static_cast<size_t>(cur_y_) * fb_w_ + cur_x_] = color;
  }
  if (++cur_x_ > win_x1_) {
    cur_x_ = win_x0_;
    ++cur_y_;
  }
}

bool P4FramebufferBus::begin(int32_t speed, int8_t dataMode) {
  _speed = speed;
  _dataMode = dataMode;
  return true;
}

void P4FramebufferBus::beginWrite() { ++write_depth_; }

void P4FramebufferBus::endWrite() {
  if (write_depth_ > 0) {
    --write_depth_;
  }
  if (write_depth_ == 0) {
    flushDirty();
  }
}

void P4FramebufferBus::writeCommand(uint8_t) {}
void P4FramebufferBus::writeCommand16(uint16_t) {}
void P4FramebufferBus::writeC8D8(uint8_t, uint8_t) {}
void P4FramebufferBus::writeC16D16(uint16_t, uint16_t) {}
void P4FramebufferBus::writeC8D16(uint8_t, uint16_t) {}
void P4FramebufferBus::writeC8D16D16(uint8_t, uint16_t, uint16_t) {}
void P4FramebufferBus::writeC8D16D16Split(uint8_t, uint16_t, uint16_t) {}
void P4FramebufferBus::batchOperation(const uint8_t*, size_t) {}

void P4FramebufferBus::write(uint8_t d) {
  // Byte stream = big-endian RGB565 (Arduino_GFX writeIndexedPixelsDouble, writeBytes).
  if (!have_msb_) {
    msb_ = d;
    have_msb_ = true;
    return;
  }
  have_msb_ = false;
  const int16_t y = cur_y_;
  putPixel(static_cast<uint16_t>((msb_ << 8) | d));
  markDirty(y, y);
}

void P4FramebufferBus::write16(uint16_t d) {
  const int16_t y = cur_y_;
  putPixel(d);
  markDirty(y, y);
}

void P4FramebufferBus::writeBytes(uint8_t* data, uint32_t len) {
  while (len--) {
    write(*data++);
  }
}

void P4FramebufferBus::writeRepeat(uint16_t p, uint32_t len) {
  if (fb_ == nullptr || len == 0) {
    return;
  }
  const int16_t y_start = cur_y_;
  // Fast path: whole rows inside the framebuffer.
  while (len > 0 && cur_y_ <= win_y1_) {
    const uint32_t row_left = static_cast<uint32_t>(win_x1_ - cur_x_ + 1);
    const uint32_t n = std::min(len, row_left);
    if (cur_y_ >= 0 && cur_y_ < fb_h_) {
      const int16_t x0 = std::max<int16_t>(0, cur_x_);
      const int16_t x1 =
          std::min<int16_t>(static_cast<int16_t>(fb_w_ - 1),
                            static_cast<int16_t>(cur_x_ + static_cast<int16_t>(n) - 1));
      uint16_t* dst = fb_ + static_cast<size_t>(cur_y_) * fb_w_;
      for (int16_t x = x0; x <= x1; ++x) {
        dst[x] = p;
      }
    }
    len -= n;
    cur_x_ = static_cast<int16_t>(cur_x_ + static_cast<int16_t>(n));
    if (cur_x_ > win_x1_) {
      cur_x_ = win_x0_;
      ++cur_y_;
    }
  }
  markDirty(y_start, static_cast<int16_t>(cur_y_));
}

void P4FramebufferBus::writePixels(uint16_t* data, uint32_t len) {
  if (fb_ == nullptr || data == nullptr || len == 0) {
    return;
  }
  const int16_t y_start = cur_y_;
  while (len > 0 && cur_y_ <= win_y1_) {
    const uint32_t row_left = static_cast<uint32_t>(win_x1_ - cur_x_ + 1);
    const uint32_t n = std::min(len, row_left);
    if (cur_y_ >= 0 && cur_y_ < fb_h_) {
      int16_t x0 = cur_x_;
      const uint16_t* src = data;
      int32_t count = static_cast<int32_t>(n);
      if (x0 < 0) {
        src += -x0;
        count += x0;
        x0 = 0;
      }
      if (x0 + count > fb_w_) {
        count = fb_w_ - x0;
      }
      if (count > 0) {
        memcpy(fb_ + static_cast<size_t>(cur_y_) * fb_w_ + x0, src,
               static_cast<size_t>(count) * sizeof(uint16_t));
      }
    }
    data += n;
    len -= n;
    cur_x_ = static_cast<int16_t>(cur_x_ + static_cast<int16_t>(n));
    if (cur_x_ > win_x1_) {
      cur_x_ = win_x0_;
      ++cur_y_;
    }
  }
  markDirty(y_start, static_cast<int16_t>(cur_y_));
  if (write_depth_ == 0) {
    flushDirty();
  }
}

// ---------------------------------------------------------------------------
// Arduino_P4Dsi
// ---------------------------------------------------------------------------

Arduino_P4Dsi::Arduino_P4Dsi(P4FramebufferBus* bus, int8_t rst, const P4DsiConfig& cfg)
    : Arduino_TFT(bus, rst, 0, true, cfg.width, cfg.height, 0, 0, 0, 0),
      fb_bus_(bus),
      cfg_(cfg) {}

void Arduino_P4Dsi::sendDcs(uint8_t cmd, const uint8_t* data, size_t len) {
  if (dbi_io_ == nullptr) {
    return;
  }
  esp_lcd_panel_io_tx_param(static_cast<esp_lcd_panel_io_handle_t>(dbi_io_), cmd, data,
                            len);
}

void Arduino_P4Dsi::tftInit() {
  if (_rst != GFX_NOT_DEFINED) {
    pinMode(_rst, OUTPUT);
    digitalWrite(_rst, HIGH);
    delay(5);
    digitalWrite(_rst, LOW);
    delay(10);
    digitalWrite(_rst, HIGH);
    delay(120);
  }

  esp_ldo_channel_handle_t ldo = nullptr;
  esp_ldo_channel_config_t ldo_cfg = {};
  ldo_cfg.chan_id = cfg_.phy_ldo_chan;
  ldo_cfg.voltage_mv = cfg_.phy_ldo_mv;
  esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &ldo);
  if (err != ESP_OK) {
    Serial.printf("DSI: PHY LDO failed: %s\n", esp_err_to_name(err));
    return;
  }

  esp_lcd_dsi_bus_config_t bus_cfg = {};
  bus_cfg.bus_id = 0;
  bus_cfg.num_data_lanes = cfg_.lanes;
  // 0 lets ESP-IDF pick the PHY PLL reference for this silicon revision
  // (pre-v3: PLL_F20M, rev3.x: XTAL) — same as Waveshare's BSP.
  bus_cfg.phy_clk_src = static_cast<mipi_dsi_phy_pllref_clock_source_t>(0);
  bus_cfg.lane_bit_rate_mbps = cfg_.lane_mbps;
  esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
  err = esp_lcd_new_dsi_bus(&bus_cfg, &dsi_bus);
  if (err != ESP_OK) {
    Serial.printf("DSI: bus failed: %s\n", esp_err_to_name(err));
    return;
  }

  esp_lcd_dbi_io_config_t dbi_cfg = {};
  dbi_cfg.virtual_channel = 0;
  dbi_cfg.lcd_cmd_bits = 8;
  dbi_cfg.lcd_param_bits = 8;
  esp_lcd_panel_io_handle_t io = nullptr;
  err = esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_cfg, &io);
  if (err != ESP_OK) {
    Serial.printf("DSI: DBI io failed: %s\n", esp_err_to_name(err));
    return;
  }
  dbi_io_ = io;

  esp_lcd_dpi_panel_config_t dpi_cfg = {};
  dpi_cfg.virtual_channel = 0;
  dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
  dpi_cfg.dpi_clock_freq_mhz = cfg_.dpi_clock_mhz;
  dpi_cfg.pixel_format = LCD_COLOR_PIXEL_FORMAT_RGB565;
  dpi_cfg.num_fbs = 1;
  dpi_cfg.video_timing.h_size = cfg_.width;
  dpi_cfg.video_timing.v_size = cfg_.height;
  dpi_cfg.video_timing.hsync_pulse_width = cfg_.hsync_pulse;
  dpi_cfg.video_timing.hsync_back_porch = cfg_.hsync_back_porch;
  dpi_cfg.video_timing.hsync_front_porch = cfg_.hsync_front_porch;
  dpi_cfg.video_timing.vsync_pulse_width = cfg_.vsync_pulse;
  dpi_cfg.video_timing.vsync_back_porch = cfg_.vsync_back_porch;
  dpi_cfg.video_timing.vsync_front_porch = cfg_.vsync_front_porch;
  dpi_cfg.flags.use_dma2d = true;

  esp_lcd_panel_handle_t panel = nullptr;
  err = esp_lcd_new_panel_dpi(dsi_bus, &dpi_cfg, &panel);
  if (err != ESP_OK) {
    Serial.printf("DSI: DPI panel failed: %s\n", esp_err_to_name(err));
    return;
  }
  dpi_panel_ = panel;

  for (size_t i = 0; i < cfg_.init_cmds_len; ++i) {
    const P4DsiInitCmd& c = cfg_.init_cmds[i];
    sendDcs(c.cmd, c.data, c.data_bytes);
    if (c.delay_ms > 0) {
      vTaskDelay(pdMS_TO_TICKS(c.delay_ms));
    }
  }

  err = esp_lcd_panel_init(panel);
  if (err != ESP_OK) {
    Serial.printf("DSI: panel init failed: %s\n", esp_err_to_name(err));
    return;
  }

  // Log the controller ID so we can confirm the init table matches the actual
  // unit (Waveshare changed the 4" round panel's chipset mid-production; the
  // table here is the JD9365 one from the XC repo, Nov 2026).
  uint8_t id[3] = {};
  if (esp_lcd_panel_io_rx_param(io, 0x04, id, sizeof(id)) == ESP_OK) {
    Serial.printf("DSI: panel ID %02X %02X %02X\n", id[0], id[1], id[2]);
  } else {
    Serial.println("DSI: panel ID read not supported");
  }

  void* fb = nullptr;
  err = esp_lcd_dpi_panel_get_frame_buffer(panel, 1, &fb);
  if (err != ESP_OK || fb == nullptr) {
    Serial.printf("DSI: no framebuffer: %s\n", esp_err_to_name(err));
    return;
  }
  fb_ = static_cast<uint16_t*>(fb);
  fb_bus_->attachFramebuffer(fb_, cfg_.width, cfg_.height);
  Serial.printf("DSI: %ux%u up, %u lanes @ %u Mbps, DPI %u MHz\n", cfg_.width, cfg_.height,
                cfg_.lanes, static_cast<unsigned>(cfg_.lane_mbps),
                static_cast<unsigned>(cfg_.dpi_clock_mhz));
}

void Arduino_P4Dsi::writeAddrWindow(int16_t x, int16_t y, uint16_t w, uint16_t h) {
  fb_bus_->setWindow(x, y, w, h);
}

void Arduino_P4Dsi::setRotation(uint8_t r) {
  // Framebuffer writes are unrotated; keep the logical size fixed at rotation 0.
  (void)r;
  Arduino_TFT::setRotation(0);
}

void Arduino_P4Dsi::invertDisplay(bool i) { sendDcs(i ? 0x21 : 0x20, nullptr, 0); }

void Arduino_P4Dsi::displayOn() {
  sendDcs(0x11, nullptr, 0);
  delay(120);
  sendDcs(0x29, nullptr, 0);
}

void Arduino_P4Dsi::displayOff() {
  sendDcs(0x28, nullptr, 0);
  delay(20);
  sendDcs(0x10, nullptr, 0);
}

void Arduino_P4Dsi::Display_Brightness(uint8_t brightness) { (void)brightness; }

#endif  // FLIGHTSCNR_BOARD_WAVESHARE_P4_4C
