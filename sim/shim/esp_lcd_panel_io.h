#pragma once
#include "esp_lcd_types.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int cmd, const void* param,
                                    size_t size);
esp_err_t esp_lcd_panel_io_rx_param(esp_lcd_panel_io_handle_t io, int cmd, void* param, size_t size);
#ifdef __cplusplus
}
#endif
