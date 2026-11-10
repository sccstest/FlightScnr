#pragma once
// Fake ESP-IDF esp_lcd MIPI-DSI API (sim only). The DPI "panel" is a heap
// framebuffer that the SDL window scans out, so the real P4 driver
// (src/hardware/p4_dsi_panel.cpp) runs unchanged.
#include <stddef.h>
#include <stdint.h>
#include "esp_sim_common.h"

struct SimLcdObj;
typedef SimLcdObj* esp_lcd_dsi_bus_handle_t;
typedef SimLcdObj* esp_lcd_panel_io_handle_t;
typedef SimLcdObj* esp_lcd_panel_handle_t;

typedef int mipi_dsi_phy_pllref_clock_source_t;
typedef int mipi_dsi_dpi_clock_source_t;
#define MIPI_DSI_DPI_CLK_SRC_DEFAULT 0
typedef enum {
  LCD_COLOR_PIXEL_FORMAT_RGB565 = 1,
  LCD_COLOR_PIXEL_FORMAT_RGB666 = 2,
  LCD_COLOR_PIXEL_FORMAT_RGB888 = 3
} lcd_color_rgb_pixel_format_t;
typedef enum { LCD_RGB_ELEMENT_ORDER_RGB = 0, LCD_RGB_ELEMENT_ORDER_BGR = 1 } lcd_rgb_element_order_t;
