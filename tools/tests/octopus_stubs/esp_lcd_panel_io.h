#pragma once
#include <cstddef>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
typedef struct fake_io *esp_lcd_panel_io_handle_t;
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io, int cmd, const void *param, size_t size);
