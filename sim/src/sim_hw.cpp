// Fake ESP32-P4 display + touch hardware for the simulator.
//  - esp_lcd: the DPI "panel" is a heap RGB565 framebuffer shown by the SDL window.
//  - Wire: a GT9271 (GT911 register map) at 0x5D whose touch point is the mouse.
// The real drivers (src/hardware/p4_dsi_panel.cpp, src/hardware/input_p4.cpp) run on top.
#include <Wire.h>
#include <esp_lcd_mipi_dsi.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>

#include <cstdarg>
#include <cstdlib>
#include <cstring>

#include "sim_state.h"

struct SimLcdObj {
  uint16_t* fb = nullptr;
  uint32_t w = 0;
  uint32_t h = 0;
};

extern "C" esp_err_t esp_lcd_new_dsi_bus(const esp_lcd_dsi_bus_config_t*,
                                         esp_lcd_dsi_bus_handle_t* out) {
  *out = new SimLcdObj();
  return ESP_OK;
}

extern "C" esp_err_t esp_lcd_new_panel_io_dbi(esp_lcd_dsi_bus_handle_t,
                                              const esp_lcd_dbi_io_config_t*,
                                              esp_lcd_panel_io_handle_t* out) {
  *out = new SimLcdObj();
  return ESP_OK;
}

extern "C" esp_err_t esp_lcd_new_panel_dpi(esp_lcd_dsi_bus_handle_t,
                                           const esp_lcd_dpi_panel_config_t* cfg,
                                           esp_lcd_panel_handle_t* out) {
  auto* p = new SimLcdObj();
  p->w = cfg->video_timing.h_size;
  p->h = cfg->video_timing.v_size;
  p->fb = static_cast<uint16_t*>(calloc(static_cast<size_t>(p->w) * p->h, sizeof(uint16_t)));
  *out = p;
  return p->fb != nullptr ? ESP_OK : ESP_ERR_NO_MEM;
}

extern "C" esp_err_t esp_lcd_dpi_panel_get_frame_buffer(esp_lcd_panel_handle_t panel,
                                                        uint32_t fb_num, void** fb0, ...) {
  (void)fb_num;
  *fb0 = panel->fb;
  sim::setFramebuffer(panel->fb, static_cast<int>(panel->w), static_cast<int>(panel->h));
  return ESP_OK;
}

extern "C" esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t, int, const void*,
                                               size_t) {
  return ESP_OK;
}

extern "C" esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t, int cmd, void* param,
                                               size_t size) {
  // RDDID (0x04): report a JD9365-style ID so the boot log line is exercised.
  static const uint8_t kId[3] = {0x93, 0x65, 0x00};
  if (cmd == 0x04 && param != nullptr) {
    memcpy(param, kId, size < 3 ? size : 3);
    return ESP_OK;
  }
  return ESP_ERR_NOT_FOUND;
}

extern "C" esp_err_t esp_lcd_panel_init(esp_lcd_panel_handle_t) { return ESP_OK; }

// ---------------------------------------------------------------------------
// Fake GT9271 on the I2C bus
// ---------------------------------------------------------------------------
namespace {
constexpr uint8_t kGtAddr = 0x5D;
bool s_status_cleared = false;
bool s_last_down = false;

uint8_t gtRegister(uint16_t reg) {
  int x = 0;
  int y = 0;
  const bool down = sim::touch(&x, &y);
  if (reg >= 0x8140 && reg <= 0x8143) {
    return static_cast<uint8_t>("9271"[reg - 0x8140]);
  }
  if (reg == 0x814E) {
    // Report a fresh frame whenever the touch state can change; the driver clears
    // the flag after each read, like on the real chip.
    if (s_status_cleared && down == s_last_down && !down) {
      return 0x00;
    }
    s_last_down = down;
    return static_cast<uint8_t>(0x80 | (down ? 1 : 0));
  }
  if (reg >= 0x814F && reg <= 0x8156) {
    const uint8_t p[8] = {0,
                          static_cast<uint8_t>(x & 0xFF),
                          static_cast<uint8_t>((x >> 8) & 0xFF),
                          static_cast<uint8_t>(y & 0xFF),
                          static_cast<uint8_t>((y >> 8) & 0xFF),
                          30,
                          0,
                          0};
    return p[reg - 0x814F];
  }
  return 0;
}
}  // namespace

TwoWire Wire;

bool TwoWire::begin(int, int, uint32_t) { return true; }

void TwoWire::beginTransmission(uint8_t addr) {
  tx_addr_ = addr;
  tx_.clear();
}

size_t TwoWire::write(uint8_t b) {
  tx_.push_back(b);
  return 1;
}

size_t TwoWire::write(const uint8_t* data, size_t n) {
  tx_.insert(tx_.end(), data, data + n);
  return n;
}

uint8_t TwoWire::endTransmission(bool) {
  if (tx_addr_ != kGtAddr) {
    return 2;  // address NACK
  }
  if (tx_.size() >= 2) {
    reg_ = static_cast<uint16_t>((tx_[0] << 8) | tx_[1]);
    if (reg_ == 0x814E && tx_.size() >= 3 && tx_[2] == 0) {
      s_status_cleared = true;
    }
  }
  return 0;
}

size_t TwoWire::requestFrom(uint8_t addr, size_t len, bool) {
  rx_.clear();
  rx_pos_ = 0;
  if (addr != kGtAddr) {
    return 0;
  }
  for (size_t i = 0; i < len; ++i) {
    rx_.push_back(gtRegister(static_cast<uint16_t>(reg_ + i)));
  }
  return len;
}

int TwoWire::available() { return static_cast<int>(rx_.size() - rx_pos_); }
int TwoWire::read() { return rx_pos_ < rx_.size() ? rx_[rx_pos_++] : -1; }
int TwoWire::peek() { return rx_pos_ < rx_.size() ? rx_[rx_pos_] : -1; }
