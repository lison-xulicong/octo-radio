// Host-side check of the main-page octopus scene (main/radio_octopus.cc). It
// runs the real renderer against a fake panel and asserts that every frame
// covers the band exactly once, uses only palette colours outside the overlay,
// actually animates, and composites the header overlay (translucent band,
// rule and text coverage) where it is asked to. Frames are written as PPM.
#include "octopus_fake_panel.h"
#include "radio_octopus_assets.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

int64_t g_fake_now_us = 1000000;

namespace {
constexpr int kW = fakelcd::kWidth;
constexpr int kTop = fakelcd::kTop;
constexpr int kH = fakelcd::kHeight;
uint16_t (&g_fb)[kW * kH] = fakelcd::framebuffer;
uint8_t (&g_written)[kW * kH] = fakelcd::written;

uint16_t native(uint16_t panel) { return static_cast<uint16_t>((panel >> 8) | (panel << 8)); }

void write_ppm(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", kW, kH);
    for (int i = 0; i < kW * kH; ++i) {
        const uint16_t v = native(g_fb[i]);
        const unsigned char rgb[3] = {static_cast<unsigned char>(((v >> 11) & 31) * 255 / 31),
                                      static_cast<unsigned char>(((v >> 5) & 63) * 255 / 63),
                                      static_cast<unsigned char>((v & 31) * 255 / 31)};
        std::fwrite(rgb, 1, 3, f);
    }
    std::fclose(f);
}

// A known overlay: the header band and rule as radio_ui sets them, plus a
// fully covered white block and a half-covered red block.
constexpr int kBandRows = 32, kBandFade = 8, kRuleY = 52;
constexpr int kBlockX = 100, kBlockY = 40, kBlockW = 20, kBlockH = 10;
void set_overlay(OctopusOverlay *ov) {
    std::memset(ov->coverage, 0, sizeof(ov->coverage));
    ov->band_rows = kBandRows;
    ov->band_fade = kBandFade;
    ov->band_alpha = 158;
    ov->band_rgb = 0x071017;
    ov->rule_y = kRuleY;
    ov->rule_x0 = 12;
    ov->rule_x1 = 228;
    ov->rule_alpha = 179;
    ov->rule_rgb = 0x24404a;
    ov->ink_count = 2;
    ov->inks[0] = {kBlockX, kBlockY, kBlockX + kBlockW, kBlockY + kBlockH, 0xffffff};
    ov->inks[1] = {10, 10, 30, 20, 0xff0000};
    for (int y = 0; y < kOverlayRows; ++y) {
        for (int x = 0; x < 240; ++x) {
            int c = 0;
            if (x >= kBlockX && x < kBlockX + kBlockW && y >= kBlockY && y < kBlockY + kBlockH) c = 15;
            if (x >= 10 && x < 30 && y >= 10 && y < 20) c = 8;
            uint8_t &pair = ov->coverage[y * kOverlayStride + x / 2];
            pair = static_cast<uint8_t>(pair | (c << ((x & 1) ? 0 : 4)));
        }
    }
}

int luminance(uint16_t v) {
    return ((v >> 11) & 31) * 2 + ((v >> 5) & 63) + (v & 31) * 2;
}
}  // namespace

int main(int argc, char **argv) {
    const std::string out = argc > 1 ? argv[1] : ".";
    int fail = 0;

    std::set<uint16_t> palette(std::begin(octopus_assets::kPalettePanel),
                               std::end(octopus_assets::kPalettePanel));

    OctopusOverlay *ov = radio_octopus_acquire();
    if (!ov) {
        std::printf("FAIL acquire\n");
        return 1;
    }
    set_overlay(ov);

    std::vector<uint16_t> first;
    int changed_frames = 0;
    for (int frame = 0; frame < 240; ++frame) {
        const bool playing = frame >= 120;
        std::memset(g_written, 0, sizeof(g_written));
        g_fake_now_us += 80000;
        if (!radio_octopus_draw_frame(playing)) {
            std::printf("FAIL frame %d returned false\n", frame);
            ++fail;
            break;
        }
        for (int y = 0; y < kH; ++y) {
            const int py = y + kTop;
            for (int x = 0; x < kW; ++x) {
                const int w = g_written[y * kW + x];
                if (w != 1) {
                    if (fail < 10) std::printf("FAIL frame %d pixel %d,%d written %d\n", frame, x, py, w);
                    ++fail;
                }
                const bool overlaid = py < kOverlayRows &&
                    (py < kBandRows + kBandFade || py == kRuleY ||
                     (x >= kBlockX && x < kBlockX + kBlockW && py >= kBlockY && py < kBlockY + kBlockH) ||
                     (x >= 10 && x < 30 && py >= 10 && py < 20));
                if (!overlaid && !palette.count(g_fb[y * kW + x])) {
                    if (fail < 10) std::printf("FAIL frame %d pixel %d,%d off-palette\n", frame, x, py);
                    ++fail;
                }
            }
        }
        // Fully covered block must be exactly the ink colour.
        for (int y = kBlockY; y < kBlockY + kBlockH; ++y) {
            for (int x = kBlockX; x < kBlockX + kBlockW; ++x) {
                if (native(g_fb[(y - kTop) * kW + x]) != 0xffff) {
                    if (fail < 10) std::printf("FAIL frame %d text block %d,%d not ink\n", frame, x, y);
                    ++fail;
                }
            }
        }
        std::vector<uint16_t> now(g_fb, g_fb + kW * kH);
        if (frame == 0) first = now;
        else if (now != first) ++changed_frames;
        if (frame == 0) write_ppm(out + "/octopus_idle_0.ppm");
        if (frame == 200) write_ppm(out + "/octopus_playing_200.ppm");
    }
    if (changed_frames < 200) {
        std::printf("FAIL scene barely animates (%d of 239 frames changed)\n", changed_frames);
        ++fail;
    }

    // Screen blanking must pause scene timing without releasing the panel
    // buffers. Keeping the frame held preserves band_guard_flush while the
    // backlight is off and makes a station change safe immediately on wake.
    const uint32_t pause_epoch = radio_octopus_epoch();
    OctopusOverlay *paused_overlay = radio_octopus_acquire();
    radio_octopus_pause();
    if (!radio_octopus_active() || radio_octopus_acquire() != paused_overlay ||
        radio_octopus_epoch() != pause_epoch) {
        std::printf("FAIL pause released the octopus buffers\n");
        ++fail;
    }
    g_fake_now_us += 30000000;
    std::memset(g_written, 0, sizeof(g_written));
    if (!radio_octopus_draw_frame(false)) {
        std::printf("FAIL draw after pause\n");
        ++fail;
    }
    uint16_t paused_row[240];
    if (!radio_octopus_capture_row(100, paused_row)) {
        std::printf("FAIL capture after pause\n");
        ++fail;
    }

    // The header band must darken the scene: compare the band rows with the
    // same frame drawn without an overlay.
    {
        long dim = 0, bright = 0;
        std::vector<uint16_t> with(g_fb, g_fb + kW * kH);
        ov->band_rows = 0;
        ov->band_fade = 0;
        ov->band_alpha = 0;
        ov->rule_y = -1;
        g_fake_now_us += 0;
        radio_octopus_draw_frame(true);
        for (int y = 0; y < kBandRows; ++y) {
            for (int x = 40; x < 90; ++x) {
                dim += luminance(native(with[y * kW + x]));
                bright += luminance(native(g_fb[y * kW + x]));
            }
        }
        if (!(dim < bright * 6 / 10)) {
            std::printf("FAIL header band does not darken the scene (%ld vs %ld)\n", dim, bright);
            ++fail;
        }
        // Below the band the darkening fades out row by row.
        long prev = -1;
        for (int y = kBandRows; y < kBandRows + kBandFade; ++y) {
            long dim_row = 0, bright_row = 0;
            for (int x = 40; x < 90; ++x) {
                dim_row += luminance(native(with[y * kW + x]));
                bright_row += luminance(native(g_fb[y * kW + x]));
            }
            const long ratio = dim_row * 1000 / std::max(bright_row, 1L);
            if (ratio >= 1000 || ratio < prev - 20) {
                std::printf("FAIL band fade row %d ratio %ld (previous %ld)\n", y, ratio, prev);
                ++fail;
            }
            prev = ratio;
        }
    }

    fail += fakelcd::bad_rects;

    // Release and re-acquire, as leaving and re-entering the main page does:
    // the overlay must come back cleared, with a new epoch.
    const uint32_t epoch = radio_octopus_epoch();
    radio_octopus_release();
    radio_octopus_release();
    ov = radio_octopus_acquire();
    if (!ov || radio_octopus_epoch() == epoch || ov->ink_count || ov->band_rows || ov->rule_y >= 0) {
        std::printf("FAIL overlay not reset after release\n");
        ++fail;
    }
    std::memset(g_written, 0, sizeof(g_written));
    g_fake_now_us += 5000000;
    if (!radio_octopus_draw_frame(false)) {
        std::printf("FAIL draw after release\n");
        ++fail;
    }
    uint16_t row[240];
    if (!radio_octopus_capture_row(100, row) || radio_octopus_capture_row(kOctopusPanelHeight, row)) {
        std::printf("FAIL capture_row bounds\n");
        ++fail;
    }
    radio_octopus_release();
    if (radio_octopus_capture_row(10, row)) {
        std::printf("FAIL capture_row after release\n");
        ++fail;
    }

    std::printf("%s: 241 frames, %d panel transfers\n", fail ? "FAILED" : "octopus ok", fakelcd::draws);
    return fail ? 1 : 0;
}
