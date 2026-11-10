#pragma once
#include "esp_lcd_types.h"

typedef struct {
  int bus_id;
  uint8_t num_data_lanes;
  mipi_dsi_phy_pllref_clock_source_t phy_clk_src;
  uint32_t lane_bit_rate_mbps;
} esp_lcd_dsi_bus_config_t;

typedef struct {
  uint8_t virtual_channel;
  int lcd_cmd_bits;
  int lcd_param_bits;
} esp_lcd_dbi_io_config_t;

typedef struct {
  uint32_t h_size;
  uint32_t v_size;
  uint32_t hsync_pulse_width;
  uint32_t hsync_back_porch;
  uint32_t hsync_front_porch;
  uint32_t vsync_pulse_width;
  uint32_t vsync_back_porch;
  uint32_t vsync_front_porch;
} esp_lcd_video_timing_t;

typedef struct {
  uint8_t virtual_channel;
  mipi_dsi_dpi_clock_source_t dpi_clk_src;
  uint32_t dpi_clock_freq_mhz;
  lcd_color_rgb_pixel_format_t pixel_format;
  uint8_t num_fbs;
  esp_lcd_video_timing_t video_timing;
  struct {
    uint32_t use_dma2d : 1;
    uint32_t disable_lp : 1;
  } flags;
} esp_lcd_dpi_panel_config_t;

#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_lcd_new_dsi_bus(const esp_lcd_dsi_bus_config_t* cfg, esp_lcd_dsi_bus_handle_t* out);
esp_err_t esp_lcd_new_panel_io_dbi(esp_lcd_dsi_bus_handle_t bus, const esp_lcd_dbi_io_config_t* cfg,
                                   esp_lcd_panel_io_handle_t* out);
esp_err_t esp_lcd_new_panel_dpi(esp_lcd_dsi_bus_handle_t bus, const esp_lcd_dpi_panel_config_t* cfg,
                                esp_lcd_panel_handle_t* out);
esp_err_t esp_lcd_dpi_panel_get_frame_buffer(esp_lcd_panel_handle_t panel, uint32_t fb_num,
                                             void** fb0, ...);
#ifdef __cplusplus
}
#endif
