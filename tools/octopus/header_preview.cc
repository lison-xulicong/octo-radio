// Renders the main page's octopus band with the real header on the host, one
// image per brand name, so a name can be judged as it will look on the device:
// same renderer, same overlay compositing, same LVGL glyph bitmaps.
//
//   header_preview <out_dir> "OCTO RADIO" "KRAKEN FM" ...
//
// Writes <out_dir>/header_<n>.ppm (240x190, the band only) and prints each
// brand's width in pixels next to the room it has before the clock.
#include "octopus_fake_panel.h"
#include "radio_header.h"

#include "lvgl.h"

#include <cstdio>
#include <string>

int64_t g_fake_now_us = 1000000;

namespace {
void write_ppm(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", fakelcd::kWidth, fakelcd::kHeight);
    for (uint16_t panel : fakelcd::framebuffer) {
        const uint16_t v = static_cast<uint16_t>((panel >> 8) | (panel << 8));
        const unsigned char rgb[3] = {static_cast<unsigned char>(((v >> 11) & 31) * 255 / 31),
                                      static_cast<unsigned char>(((v >> 5) & 63) * 255 / 63),
                                      static_cast<unsigned char>((v & 31) * 255 / 31)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

int text_width(const lv_font_t *font, const char *text) {
    int width = 0;
    lv_font_glyph_dsc_t g;
    for (const char *p = text; *p; ++p) {
        if (lv_font_get_glyph_dsc(font, &g, static_cast<unsigned char>(*p),
                                  static_cast<unsigned char>(p[1]))) {
            width += g.adv_w;
        }
    }
    return width;
}
}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s out_dir name...\n", argv[0]);
        return 2;
    }
    lv_init();
    RadioHeaderText text;
    std::snprintf(text.clock, sizeof(text.clock), "08:42");
    std::snprintf(text.battery, sizeof(text.battery), "87%%");
    text.network = true;
    // The clock is right-aligned to x 173; keep a gap of 8 px before it.
    const int room = 173 - text_width(&lv_font_montserrat_20, "88:88") - 8 - 12;

    for (int n = 2; n < argc; ++n) {
        radio_octopus_test_reset(1.0f);
        OctopusOverlay *ov = radio_octopus_acquire();
        if (!ov) return 1;
        radio_header_paint(ov, text, argv[n]);
        for (int frame = 0; frame < 40; ++frame) {
            g_fake_now_us += 80000;
            radio_octopus_draw_frame(true);
        }
        write_ppm(std::string(argv[1]) + "/header_" + std::to_string(n - 2) + ".ppm");
        const int width = text_width(&lv_font_montserrat_14, argv[n]);
        std::printf("%-14s %3d px of %d%s\n", argv[n], width, room, width > room ? "  TOO WIDE" : "");
        radio_octopus_release();
    }
    return 0;
}
