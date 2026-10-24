#include "octopus_fake_panel.h"

#include <cstdio>

namespace fakelcd {
uint16_t framebuffer[kWidth * kHeight];
uint8_t written[kWidth * kHeight];
int bad_rects = 0;
int draws = 0;
}  // namespace fakelcd

esp_lcd_panel_handle_t bsp_display_panel() {
    return reinterpret_cast<esp_lcd_panel_handle_t>(1);
}
esp_lcd_panel_io_handle_t bsp_display_io() {
    return reinterpret_cast<esp_lcd_panel_io_handle_t>(1);
}
esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t, int, const void *, size_t) {
    return ESP_OK;
}

esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t, int x0, int y0, int x1, int y1,
                                    const void *data) {
    using namespace fakelcd;
    ++draws;
    if (x0 < 0 || x1 > kWidth || x0 >= x1 || y0 < kTop || y1 > kTop + kHeight || y0 >= y1) {
        std::printf("FAIL draw rect out of band: %d,%d - %d,%d\n", x0, y0, x1, y1);
        ++bad_rects;
        return ESP_FAIL;
    }
    const auto *src = static_cast<const uint16_t *>(data);
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const int i = (y - kTop) * kWidth + x;
            framebuffer[i] = *src++;
            ++written[i];
        }
    }
    return ESP_OK;
}
