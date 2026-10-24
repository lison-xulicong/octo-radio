#include "radio_header.h"

#include "lvgl.h"
#include "radio_font.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint32_t kBackground = 0x071017;
constexpr uint32_t kText = 0xF4F0DE;
constexpr uint32_t kMuted = 0x849BA0;
constexpr uint32_t kAmber = 0xFFB74D;
constexpr uint32_t kGreen = 0x4ED39A;
constexpr uint32_t kRed = 0xFF5D62;

// The band is solid for kBandRows rows, then fades out over kBandFade rows so
// it has no hard lower edge.
constexpr int kBandRows = 32;
constexpr int kBandFade = 12;
constexpr uint8_t kBandAlpha = 158;  // 62%, as in the simulator
// Brand, clock, signal bars and battery all stand on this row (the first row
// below the glyphs), whatever their font.
constexpr int kBaseline = 24;
constexpr int kChannelY = 44;
constexpr int kSleepTimerRight = 227;
constexpr int kSignalX = 178;
constexpr int kBatteryRight = 234;
constexpr int kClockRight = 173;

int decode_utf8(const char *text, uint32_t *out, int max) {
    int n = 0;
    const auto *p = reinterpret_cast<const unsigned char *>(text);
    while (*p && n < max) {
        uint32_t c = *p++;
        int extra = 0;
        if (c >= 0xf0) { c &= 0x07; extra = 3; }
        else if (c >= 0xe0) { c &= 0x0f; extra = 2; }
        else if (c >= 0xc0) { c &= 0x1f; extra = 1; }
        for (; extra > 0 && (*p & 0xc0) == 0x80; --extra) c = (c << 6) | (*p++ & 0x3f);
        out[n++] = c;
    }
    return n;
}

void add_ink(OctopusOverlay *ov, int x0, int y0, int x1, int y1, uint32_t rgb) {
    if (ov->ink_count >= kOverlayInks) return;
    ov->inks[ov->ink_count++] = {static_cast<int16_t>(x0), static_cast<int16_t>(y0),
                                 static_cast<int16_t>(x1), static_cast<int16_t>(y1), rgb};
}

inline void cover(OctopusOverlay *ov, int x, int y, int value) {
    if (x < 0 || y < 0 || x >= 240 || y >= kOverlayRows || value <= 0) return;
    uint8_t &pair = ov->coverage[y * kOverlayStride + x / 2];
    const int shift = (x & 1) ? 0 : 4;
    const int old = (pair >> shift) & 15;
    if (value > old) pair = static_cast<uint8_t>((pair & ~(15 << shift)) | (std::min(value, 15) << shift));
}

// Line top that puts the font's baseline on kBaseline.
int top_for_baseline(const lv_font_t *font) {
    return kBaseline - (font->line_height - font->base_line);
}

// Height of the font's capitals, for sizing the signal bars to the text.
int cap_height(const lv_font_t *font) {
    lv_font_glyph_dsc_t g;
    return lv_font_get_glyph_dsc(font, &g, 'H', 0) ? g.box_h + g.ofs_y : font->line_height / 2;
}

// Draws text into the overlay the way an LVGL label would at (x, y), using the
// font's own 4-bit glyphs. With right >= 0 the text is right-aligned to it
// instead. Returns the text width.
int paint_text(OctopusOverlay *ov, const lv_font_t *font, const char *text, int x, int y,
               uint32_t rgb, int right = -1) {
    uint32_t cps[32];
    const int n = decode_utf8(text, cps, 32);
    lv_font_glyph_dsc_t g;
    int width = 0;
    for (int i = 0; i < n; ++i) {
        if (lv_font_get_glyph_dsc(font, &g, cps[i], i + 1 < n ? cps[i + 1] : 0)) width += g.adv_w;
    }
    if (right >= 0) x = right - width;
    const int line_height = lv_font_get_line_height(font);
    add_ink(ov, x - 1, y, x + width + 1, y + line_height, rgb);

    int pen = x;
    for (int i = 0; i < n; ++i) {
        if (!lv_font_get_glyph_dsc(font, &g, cps[i], i + 1 < n ? cps[i + 1] : 0)) continue;
        const lv_font_t *source = g.resolved_font ? g.resolved_font : font;
        const auto *fd = static_cast<const lv_font_fmt_txt_dsc_t *>(source->dsc);
        if (fd && fd->bpp == 4 && fd->bitmap_format == LV_FONT_FMT_TXT_PLAIN && g.gid.index &&
            g.box_w && g.box_h) {
            const lv_font_fmt_txt_glyph_dsc_t &gd = fd->glyph_dsc[g.gid.index];
            const uint8_t *bits = &fd->glyph_bitmap[gd.bitmap_index];
            const int gx = pen + g.ofs_x;
            const int gy = y + (font->line_height - font->base_line) - g.box_h - g.ofs_y;
            for (int row = 0; row < g.box_h; ++row) {
                for (int col = 0; col < g.box_w; ++col) {
                    int value;
                    if (g.stride) {
                        const uint8_t byte = bits[row * g.stride + col / 2];
                        value = (col & 1) ? (byte & 15) : (byte >> 4);
                    } else {
                        const int i_px = row * g.box_w + col;
                        const uint8_t byte = bits[i_px / 2];
                        value = (i_px & 1) ? (byte & 15) : (byte >> 4);
                    }
                    cover(ov, gx + col, gy + row, value);
                }
            }
        }
        pen += g.adv_w;
    }
    return width;
}

void paint_box(OctopusOverlay *ov, int x, int y, int w, int h, int value, uint32_t rgb) {
    add_ink(ov, x, y, x + w, y + h, rgb);
    for (int yy = y; yy < y + h; ++yy) {
        for (int xx = x; xx < x + w; ++xx) cover(ov, xx, yy, value);
    }
}

}  // namespace

void radio_header_paint(OctopusOverlay *ov, const RadioHeaderText &text, const char *brand) {
    std::memset(ov->coverage, 0, sizeof(ov->coverage));
    ov->ink_count = 0;
    ov->band_rows = kBandRows;
    ov->band_fade = kBandFade;
    ov->band_alpha = kBandAlpha;
    ov->band_rgb = kBackground;
    ov->rule_y = -1;

    const lv_font_t *small = &lv_font_montserrat_14;
    const lv_font_t *large = &lv_font_montserrat_20;
    paint_text(ov, small, brand, 12, top_for_baseline(small), kAmber);
    paint_text(ov, large, text.clock, 0, top_for_baseline(large), kText, kClockRight);
    // Signal bars rise to the capitals' height. Disconnected bars are dimmed
    // to 30%, as the LVGL version was.
    const int cap = cap_height(small);
    for (int i = 0; i < 3; ++i) {
        const int height = cap * (i + 1) / 3;
        paint_box(ov, kSignalX + i * 5, kBaseline - height, 3, height, text.network ? 15 : 5,
                  text.network ? kGreen : kMuted);
    }
    paint_text(ov, small, text.battery, 0, top_for_baseline(small),
               text.battery_low ? kRed : kMuted, kBatteryRight);
    paint_text(ov, small, text.channel, 13, kChannelY, kAmber);
    if (text.sleep[0]) {
        paint_text(ov, &radio_font, text.sleep, 0, kChannelY - 1, kAmber, kSleepTimerRight);
    }
}
