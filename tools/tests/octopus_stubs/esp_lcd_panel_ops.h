#pragma once
#include "esp_lcd_panel_io.h"
typedef struct fake_panel *esp_lcd_panel_handle_t;
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x_start, int y_start, int x_end, int y_end, const void *data);
