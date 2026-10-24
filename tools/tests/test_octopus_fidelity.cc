// Compares the firmware renderer with the design (the simulator's scene) at
// fixed moments. The design frames are produced by tools/octopus/reference.html;
// this renders the same moments through main/radio_octopus.cc against a fake
// panel and reports how far apart they are, per region.
//
//   fidelity <reference_frames.json> <outdir> [--gate]
//
// Without --gate it only reports. With --gate it fails if any region is worse
// than its limit, which is how the renderer is kept honest as it changes.
#define RADIO_OCTOPUS_TESTING 1
#include "octopus_fake_panel.h"
#include "radio_octopus_assets.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

int64_t g_fake_now_us = 1000000;

namespace {
namespace A = octopus_assets;
constexpr int kW = A::kWidth;   // scene px
constexpr int kH = A::kHeight;

struct RefFrame {
    int clock;
    std::string mode;
    std::vector<int> pix;
};

// Minimal reader for the exporter's fixed output shape.
std::vector<RefFrame> load_reference(const std::string &path) {
    std::ifstream in(path);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<RefFrame> frames;
    size_t pos = s.find("\"frames\"");
    while ((pos = s.find("{\"clock\":", pos)) != std::string::npos) {
        RefFrame f;
        f.clock = std::atoi(s.c_str() + pos + 9);
        size_t m = s.find("\"mode\":\"", pos) + 8;
        f.mode = s.substr(m, s.find('"', m) - m);
        size_t p = s.find("\"pix\":[", pos) + 7;
        size_t end = s.find(']', p);
        const char *c = s.c_str() + p;
        const char *stop = s.c_str() + end;
        while (c < stop) {
            char *next;
            f.pix.push_back(static_cast<int>(std::strtol(c, &next, 10)));
            c = next;
            if (*c == ',') ++c;
        }
        frames.push_back(std::move(f));
        pos = end;
    }
    return frames;
}

// Firmware frame -> palette index per scene pixel (the panel shows each scene
// pixel as a 2x2 block of a palette colour).
std::vector<int> firmware_indices() {
    std::vector<int> out(kW * kH);
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const uint16_t v = fakelcd::framebuffer[(y * 2) * fakelcd::kWidth + x * 2];
            int idx = -1;
            for (int i = 0; i < A::kPaletteSize; ++i) {
                if (A::kPalettePanel[i] == v) { idx = i; break; }
            }
            out[y * kW + x] = idx;
        }
    }
    return out;
}

void rgb_of(int idx, int &r, int &g, int &b) {
    const uint16_t c = A::kPalette565[idx];
    r = ((c >> 11) & 31) * 255 / 31;
    g = ((c >> 5) & 63) * 255 / 63;
    b = (c & 31) * 255 / 31;
}

// "Where is the octopus" as a mask, from palette names that only the body and
// arms use. The water palette is entries 0..10.
bool is_octopus(int idx) { return idx >= 11 && idx <= 31; }


struct Breakdown { long total = 0, same = 0; };
Breakdown g_cat[4];  // water, arms+body, sunglasses/sparkle, suckers
int category(int idx) {
    if (idx >= 0 && idx <= 10) return 0;
    if (idx >= 25 && idx <= 31) return idx == 25 || idx == 26 ? 3 : 2;  // gA/gB .. k*
    return 1;
}


// Water-only match by scene row band, to tell caustics (top), beams (middle)
// and vignette (bottom) apart.
long g_row_total[8], g_row_same[8];
double g_row_err[8];
struct Score {
    double exact = 0;       // fraction of pixels with the same palette index
    double mean_rgb = 0;    // mean per-channel error, 0..255
    double arm_iou = 0;     // overlap of the octopus silhouettes
    int n = 0;
};

Score compare(const std::vector<int> &fw, const std::vector<int> &ref, int y0, int y1) {
    Score s;
    long same = 0, inter = 0, uni = 0;
    double err = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = 0; x < kW; ++x) {
            const int a = fw[y * kW + x], b = ref[y * kW + x];
            if (a < 0 || b < 0) continue;
            ++s.n;
            if (a == b) ++same;
            { Breakdown &c = g_cat[category(b)]; ++c.total; if (a == b) ++c.same; }
            if (category(b) == 0) { const int band = std::min(7, y * 8 / kH); ++g_row_total[band]; if (a == b) ++g_row_same[band];
                int ar2, ag2, ab2, br2, bg2, bb2; rgb_of(a, ar2, ag2, ab2); rgb_of(b, br2, bg2, bb2);
                g_row_err[band] += (std::abs(ar2 - br2) + std::abs(ag2 - bg2) + std::abs(ab2 - bb2)) / 3.0; }
            int ar, ag, ab, br, bg, bb;
            rgb_of(a, ar, ag, ab);
            rgb_of(b, br, bg, bb);
            err += (std::abs(ar - br) + std::abs(ag - bg) + std::abs(ab - bb)) / 3.0;
            const bool oa = is_octopus(a), ob = is_octopus(b);
            if (oa && ob) ++inter;
            if (oa || ob) ++uni;
        }
    }
    if (s.n) { s.exact = static_cast<double>(same) / s.n; s.mean_rgb = err / s.n; }
    s.arm_iou = uni ? static_cast<double>(inter) / uni : 1.0;
    return s;
}

void write_ppm(const std::string &path, const std::vector<int> &pix, const std::vector<int> *diff_against) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", kW, kH);
    for (int i = 0; i < kW * kH; ++i) {
        int r = 0, g = 0, b = 0;
        if (pix[i] >= 0) rgb_of(pix[i], r, g, b);
        if (diff_against && pix[i] != (*diff_against)[i]) { r = 255; g = 0; b = 255; }
        const unsigned char c[3] = {static_cast<unsigned char>(r), static_cast<unsigned char>(g), static_cast<unsigned char>(b)};
        std::fwrite(c, 1, 3, f);
    }
    std::fclose(f);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 3) { std::printf("usage: fidelity <ref.json> <outdir> [--gate]\n"); return 2; }
    const bool gate = argc > 3 && std::string(argv[3]) == "--gate";
    const std::string out = argv[2];
    const auto refs = load_reference(argv[1]);
    if (refs.empty()) { std::printf("FAIL: no reference frames\n"); return 2; }

    int fail = 0;

    std::printf("%-8s %5s | %7s %8s %8s  %8s\n", "mode", "clock", "exact", "rgbErr", "octoIoU", "hash");
    double worst_iou = 1, sum_exact = 0, sum_rgb = 0;
    for (const RefFrame &ref : refs) {
        const float mix = ref.mode == "playing" ? 1.0f : 0.0f;
        radio_octopus_test_reset(mix);
        fakelcd::draws = 0;
        std::memset(fakelcd::written, 0, sizeof(fakelcd::written));
        g_fake_now_us = 1000000;
        radio_octopus_draw_frame(ref.mode == "playing");  // t = 0
        if (ref.clock > 0) {
            // One call may advance at most 200 ms, so walk to the target in
            // steps of about 3 simulator frames (1 frame = 55.6 ms).
            const int steps = std::max(1, static_cast<int>(std::ceil(ref.clock / 3.0)));
            const int64_t total_us = std::llround(ref.clock / 0.018 * 1000.0);
            for (int k = 0; k < steps; ++k) {
                g_fake_now_us += total_us / steps;
                radio_octopus_draw_frame(ref.mode == "playing");
            }
        }
        const auto fw = firmware_indices();
        const Score s = compare(fw, ref.pix, 0, kH);
        // FNV-1a of the panel contents, so a refactor can prove it left a
        // variant's output untouched.
        uint32_t hash = 2166136261u;
        for (int i = 0; i < fakelcd::kWidth * fakelcd::kHeight; ++i) {
            hash = (hash ^ (fakelcd::framebuffer[i] & 0xff)) * 16777619u;
            hash = (hash ^ (fakelcd::framebuffer[i] >> 8)) * 16777619u;
        }
        std::printf("%-8s %5d | %6.1f%% %8.2f %7.1f%%  %08x\n", ref.mode.c_str(), ref.clock,
                    s.exact * 100, s.mean_rgb, s.arm_iou * 100, hash);
        sum_exact += s.exact;
        sum_rgb += s.mean_rgb;
        worst_iou = std::min(worst_iou, s.arm_iou);
        char name[160];
        std::snprintf(name, sizeof name, "%s/fw_%s_%03d.ppm", out.c_str(), ref.mode.c_str(), ref.clock);
        write_ppm(name, fw, &ref.pix);
        std::snprintf(name, sizeof name, "%s/ref_%s_%03d.ppm", out.c_str(), ref.mode.c_str(), ref.clock);
        write_ppm(name, ref.pix, nullptr);
    }
    std::printf("mean exact %.1f%%  mean rgbErr %.2f  worst octopus IoU %.1f%%\n",
                sum_exact / refs.size() * 100, sum_rgb / refs.size(), worst_iou * 100);
    {
        const char *names[4] = {"water", "body+arms", "sunglasses", "gold rim/other"};
        for (int i = 0; i < 4; ++i) {
            if (g_cat[i].total) std::printf("  %-14s %7ld px  match %5.1f%%\n", names[i], g_cat[i].total, 100.0 * g_cat[i].same / g_cat[i].total);
        }
    }
    std::printf("  water by row band (top -> bottom, each ~9 rows):");
    for (int i = 0; i < 8; ++i) std::printf(" %4.0f%%", g_row_total[i] ? 100.0 * g_row_same[i] / g_row_total[i] : 0.0);
    std::printf("\n  water rgbErr by band:                     ");
    for (int i = 0; i < 8; ++i) std::printf(" %5.1f", g_row_total[i] ? g_row_err[i] / g_row_total[i] : 0.0);
    std::printf("\n");
    if (gate && (worst_iou < 0.90 || sum_rgb / refs.size() > 12.5)) {
        std::printf("FAIL: firmware drifted from the design\n");
        ++fail;
    }
    return fail ? 1 : 0;
}
