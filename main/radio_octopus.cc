#include "radio_octopus.h"

#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#include "radio_octopus_assets.h"
#include "bsp_display.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// The scene follows the simulator (octopus-scene.core.js). Water and the
// octopus body are pre-rendered by tools/octopus; the caustics, light beams,
// arms, bubbles, waterline, drifting particles, crest glint and lens sparkle
// are animated here.
//
// ESP32-C3 specifics that shape this file: there is no FPU, so per-pixel work
// is integer and float is limited to a few dozen values per frame; code and
// constant data run through a small flash cache, so the hottest loops live in
// IRAM and the colour lookup table is a 4 KB table in RAM rather than a large
// one in flash.

// Quality comes from menuconfig (RADIO_OCTOPUS_QUALITY_*) on the device. Host
// tests pass -DRADIO_OCTOPUS_LITE=1 to build the lite path.
#if defined(CONFIG_RADIO_OCTOPUS_QUALITY_LITE) && !defined(RADIO_OCTOPUS_LITE)
#define RADIO_OCTOPUS_LITE 1
#endif

namespace {

namespace A = octopus_assets;

#ifdef RADIO_OCTOPUS_LITE
constexpr bool kLite = true;
#else
constexpr bool kLite = false;
#endif

constexpr const char *kTag = "octopus";
constexpr int kW = A::kWidth;
constexpr int kH = A::kHeight;
constexpr int kPanelWidth = kW * 2;
constexpr int kChunkRows = 6;  // scene rows per panel transfer
constexpr int kSegments = 9;
constexpr int kSamplesPerSegment = 20;
constexpr int kArmSamples = kSegments * kSamplesPerSegment + 1;
constexpr float kTwoPi = 6.28318531f;
constexpr float kHalfPi = 1.57079633f;
constexpr float kPhaseUnits = 65536.0f / kTwoPi;  // radians -> 1/65536 turns

static_assert(kPanelWidth == 240 && kOctopusPanelHeight == kH * 2, "scene and band disagree");

uint8_t *s_work;           // kW x kH, palette index
uint16_t *s_out;           // panel stripe, byte-swapped RGB565, DMA capable
OctopusOverlay *s_overlay;
bool s_out_pending;
bool s_tables_ready;
bool s_frame_held;         // s_work holds a finished frame
int64_t s_retry_after_us;
uint32_t s_epoch;
uint32_t s_last_frame_us;
int s_body_x0;
int s_body_y0;
int s_body_x1;
int s_body_y1;

int16_t s_sin[257];                          // Q14, one full turn
int16_t s_basis[kSamplesPerSegment + 1][4];  // Catmull-Rom weights, Q12
uint16_t s_taper[kArmSamples];               // (1 - t)^1.35, Q12
uint16_t s_arm_color[8][9];                  // t0..t5, sW, sS, sD after depth fog
uint16_t s_circle[257];                      // sqrt(1 - s^2), Q8, s in 1/256
uint16_t s_bloom_level[257];                 // light-source bloom by squared distance, Q8
uint16_t s_glow_level[257];                  // linear radial falloff by squared distance, Q8
uint16_t s_beam_level[257];                  // beam brightness along its length, Q8
uint16_t s_pal565[A::kPaletteSize];          // palette, native
uint16_t s_pal_panel[A::kPaletteSize];       // palette, panel byte order
uint16_t s_pal_band[A::kPaletteSize];        // palette under the header band, panel order
uint8_t s_lut[4096];                         // RGB444 -> nearest palette index

// Per-frame arm geometry (Q8 scene px), kept off the LVGL task stack.
[[maybe_unused]] int32_t s_px[kArmSamples], s_py[kArmSamples];
[[maybe_unused]] int16_t s_nx[kArmSamples], s_ny[kArmSamples];  // unit normal, Q8
[[maybe_unused]] uint16_t s_seg[kArmSamples];                   // length from previous sample, Q8

float s_clock;  // simulator frames (18 per second)
int64_t s_last_us;
float s_phase[8];
float s_mix;  // 0 idle .. 1 playing, eased
bool s_mix_pinned;  // host tests only

uint32_t s_stat_frames;
int64_t s_stat_us;
int64_t s_stat_max_us;
// Time per drawing phase, summed over the stats window. Logged with the frame
// average so the slow part can be read off the device.
enum Phase { kPhaseBackground, kPhaseCaustics, kPhaseRays, kPhaseParticles, kPhaseBubbles,
             kPhaseArms, kPhaseBody, kPhaseGlint, kPhaseSparkle, kPhaseSend, kPhaseCount };
int64_t s_phase_us[kPhaseCount];

// Back-to-front: outer arms first, inner arms last (and brightest).
constexpr int kDrawOrder[8] = {0, 7, 1, 6, 2, 5, 3, 4};

struct Bubble {
    float x, r, sp, hx, hf, sd;
};
constexpr Bubble kBubbles[8] = {
    {14, 2.6f, 0.52f, 2.2f, 0.062f, 0},  {29, 1.5f, 0.78f, 1.4f, 0.094f, 37},
    {44, 2.0f, 0.64f, 1.8f, 0.071f, 71}, {58, 1.2f, 0.92f, 1.1f, 0.108f, 19},
    {72, 2.3f, 0.47f, 2.5f, 0.055f, 94}, {86, 1.7f, 0.71f, 1.6f, 0.086f, 52},
    {99, 2.9f, 0.41f, 2.8f, 0.049f, 12}, {111, 1.4f, 0.85f, 1.3f, 0.101f, 66},
};
constexpr float kBubbleStartY = kH + 4;
constexpr float kBurstFrames = 16;

// ── small helpers ──────────────────────────────────────────

constexpr uint16_t to565(uint32_t rgb) {
    return static_cast<uint16_t>(((rgb >> 8) & 0xf800) | ((rgb >> 5) & 0x07e0) |
                                 ((rgb >> 3) & 0x001f));
}

inline uint16_t swap16(uint16_t v) { return static_cast<uint16_t>((v >> 8) | (v << 8)); }

inline void unpack(uint16_t c, int &r, int &g, int &b) {
    r = ((c >> 8) & 0xf8) | (c >> 13);
    g = ((c >> 3) & 0xfc) | ((c >> 9) & 0x03);
    b = ((c << 3) & 0xf8) | ((c >> 2) & 0x07);
}

inline uint16_t pack(int r, int g, int b) {
    return static_cast<uint16_t>(((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
}

inline uint8_t quantize(uint16_t c) {
    return s_lut[((c >> 4) & 0xf00) | ((c >> 3) & 0x0f0) | ((c >> 1) & 0x00f)];
}

// Mix native RGB565 `c` toward `rgb` by a/256.
inline uint16_t mix(uint16_t c, uint32_t rgb, int a) {
    int r, g, b;
    unpack(c, r, g, b);
    r += ((static_cast<int>((rgb >> 16) & 0xff) - r) * a) >> 8;
    g += ((static_cast<int>((rgb >> 8) & 0xff) - g) * a) >> 8;
    b += ((static_cast<int>(rgb & 0xff) - b) * a) >> 8;
    return pack(r, g, b);
}

inline uint32_t to_phase(float radians) {
    return static_cast<uint32_t>(static_cast<int32_t>(radians * kPhaseUnits));
}

// Q14 sine of a phase in 1/65536 turns.
inline int isin(uint32_t phase) {
    const uint32_t i = (phase >> 8) & 255;
    const int f = static_cast<int>(phase & 255);
    return s_sin[i] + (((s_sin[i + 1] - s_sin[i]) * f) >> 8);
}

inline float fsin(float x) { return isin(to_phase(x)) * (1.0f / 16384.0f); }
inline float fcos(float x) { return fsin(x + kHalfPi); }

inline int alpha8(float a) { return a <= 0.0f ? 0 : a >= 1.0f ? 256 : static_cast<int>(a * 256.0f); }

[[maybe_unused]] uint32_t isqrt(uint32_t v) {
    uint32_t r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

inline int floor8(int32_t v) { return v >> 8; }
inline int ceil8(int32_t v) { return -((-v) >> 8); }

inline void plot(int x, int y, uint16_t color) {
    if (x < 0 || y < 0 || x >= kW || y >= kH) return;
    s_work[y * kW + x] = quantize(color);
}

// Alpha blend one pixel toward `rgb`; a in 0..256.
[[maybe_unused]] IRAM_ATTR void blend(int x, int y, uint32_t rgb, int a) {
    if (x < 0 || y < 0 || x >= kW || y >= kH || a <= 0) return;
    uint8_t &p = s_work[y * kW + x];
    p = quantize(mix(s_pal565[p], rgb, a));
}

inline void blend_f(float x, float y, uint32_t rgb, float alpha) {
    blend(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(y)), rgb, alpha8(alpha));
}

// Brighten a pixel additively by (r, g, b) / 256 of full scale.
IRAM_ATTR inline void add(uint8_t &p, int ar, int ag, int ab) {
    int r, g, b;
    unpack(s_pal565[p], r, g, b);
    p = quantize(pack(std::min(255, r + ar), std::min(255, g + ag),
                      std::min(255, b + ab)));
}

// Filled disc; centre and radius in Q8 scene px. The half-width of each row
// comes from a table, so a disc costs one division instead of a square root
// per row.
[[maybe_unused]] IRAM_ATTR void disc(int32_t cx, int32_t cy, int32_t r, uint16_t color) {
    if (r <= 0) return;
    const int32_t inv_r = (1 << 24) / r;  // 256 / r in Q16
    const int y0 = std::max(0, ceil8(cy - r - 128));
    const int y1 = std::min(kH - 1, floor8(cy + r - 128));
    for (int y = y0; y <= y1; ++y) {
        const int32_t dy = std::abs(y * 256 + 128 - cy);
        if (dy > r) continue;
        const int32_t s = std::min<int32_t>(256, (dy * inv_r) >> 16);
        const int32_t hw = (r * s_circle[s]) >> 8;
        const int x0 = std::max(0, ceil8(cx - hw - 128));
        const int x1 = std::min(kW - 1, floor8(cx + hw - 128));
        uint8_t *row = s_work + y * kW;
        const uint8_t index = quantize(color);
        for (int x = x0; x <= x1; ++x) row[x] = index;
    }
}

// Squared distance in Q16 (Q8 squared) scaled to 0..256 of the radius
// squared: index into the *_level tables.
inline int radial_index(int32_t d2, uint32_t inv_r2) {
    const uint64_t t = (static_cast<uint64_t>(d2) * inv_r2) >> 32;
    return t > 256 ? 257 : static_cast<int>(t);
}
inline uint32_t inv_radius2(float radius) {
    const double r_q8 = radius * 256.0;
    return static_cast<uint32_t>((256.0 * 4294967296.0) / (r_q8 * r_q8));
}

uint16_t fog_mix(uint32_t rgb, float f) {
    const uint32_t d = A::kRgb_bg3;
    auto ch = [&](int shift) {
        const float c = static_cast<float>((rgb >> shift) & 0xff);
        const float t = static_cast<float>((d >> shift) & 0xff);
        return static_cast<uint32_t>(c + (t - c) * f + 0.5f);
    };
    return to565((ch(16) << 16) | (ch(8) << 8) | ch(0));
}

void init_tables() {
    for (int i = 0; i <= 256; ++i) {
        s_sin[i] = static_cast<int16_t>(std::lround(std::sin(i * kTwoPi / 256.0) * 16384.0));
        const double s = i / 256.0;
        s_circle[i] = static_cast<uint16_t>(std::lround(std::sqrt(std::max(0.0, 1.0 - s * s)) * 256.0));
        // Tables indexed by squared distance: d = sqrt(i / 256).
        const double d = std::sqrt(s);
        // The design's bloom gradient: 0.40 at the centre, 0.17 at 35% of the
        // radius, 0 at the rim, linear in between.
        const double bloom = d < 0.35 ? 0.40 - 0.657 * d : 0.17 * (1.0 - (d - 0.35) / 0.65);
        s_bloom_level[i] = static_cast<uint16_t>(std::lround(std::max(0.0, bloom) * 256.0));
        s_glow_level[i] = static_cast<uint16_t>(std::lround((1.0 - d) * 256.0));
        // Beam gradient stops along its length: 0.52, 0.40, 0.17, 0.05, 0.
        const double f = s;
        double level;
        if (f < 0.14) level = 0.52 + (0.40 - 0.52) * (f / 0.14);
        else if (f < 0.42) level = 0.40 + (0.17 - 0.40) * ((f - 0.14) / 0.28);
        else if (f < 0.74) level = 0.17 + (0.05 - 0.17) * ((f - 0.42) / 0.32);
        else level = 0.05 * (1.0 - (f - 0.74) / 0.26);
        s_beam_level[i] = static_cast<uint16_t>(std::lround(std::max(0.0, level) * 256.0));
    }
    for (int k = 0; k <= kSamplesPerSegment; ++k) {
        const double t = static_cast<double>(k) / kSamplesPerSegment;
        const double t2 = t * t, t3 = t2 * t;
        const double w[4] = {0.5 * (-t3 + 2 * t2 - t), 0.5 * (3 * t3 - 5 * t2 + 2),
                             0.5 * (-3 * t3 + 4 * t2 + t), 0.5 * (t3 - t2)};
        for (int j = 0; j < 4; ++j) s_basis[k][j] = static_cast<int16_t>(std::lround(w[j] * 4096));
    }
    for (int i = 0; i < kArmSamples; ++i) {
        const double t = static_cast<double>(i) / (kArmSamples - 1);
        s_taper[i] = static_cast<uint16_t>(std::lround(std::pow(1.0 - t, 1.35) * 4096));
    }
    const uint32_t base[9] = {A::kRgb_t0, A::kRgb_t1, A::kRgb_t2, A::kRgb_t3, A::kRgb_t4,
                              A::kRgb_t5, A::kRgb_sW, A::kRgb_sS, A::kRgb_sD};
    for (int pos = 0; pos < 8; ++pos) {
        // Further-back arms are pulled toward the deep-water colour.
        const float fog = (1.0f - static_cast<float>(pos) / 7.0f) * 0.42f;
        for (int c = 0; c < 9; ++c) s_arm_color[kDrawOrder[pos]][c] = fog_mix(base[c], fog);
    }
    for (int i = 0; i < A::kPaletteSize; ++i) {
        s_pal565[i] = A::kPalette565[i];
        s_pal_panel[i] = A::kPalettePanel[i];
    }
    s_body_x0 = kW;
    s_body_y0 = kH;
    s_body_x1 = -1;
    s_body_y1 = -1;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            if (A::kBody[y * kW + x] == A::kTransparent) continue;
            s_body_x0 = std::min(s_body_x0, x);
            s_body_y0 = std::min(s_body_y0, y);
            s_body_x1 = std::max(s_body_x1, x);
            s_body_y1 = std::max(s_body_y1, y);
        }
    }
    // Nearest palette entry for each RGB444 cell, measured from the cell's
    // centre colour.
    for (int v = 0; v < 4096; ++v) {
        const int r = ((v >> 8) & 15) * 17, g = ((v >> 4) & 15) * 17, b = (v & 15) * 17;
        int best = 0, best_d = 1 << 30;
        for (int i = 0; i < A::kPaletteSize; ++i) {
            int pr, pg, pb;
            unpack(s_pal565[i], pr, pg, pb);
            const int d = (r - pr) * (r - pr) + (g - pg) * (g - pg) + (b - pb) * (b - pb);
            if (d < best_d) {
                best_d = d;
                best = i;
            }
        }
        s_lut[v] = static_cast<uint8_t>(best);
    }
    for (int i = 0; i < 8; ++i) s_phase[i] = A::kArmAnim[i].phase;
    s_tables_ready = true;
}

// ── scene layers ───────────────────────────────────────────

inline bool x_in_body(int x, int y) {
    return x >= 0 && y >= 0 && x < kW && y < kH && A::kBody[y * kW + x] != A::kTransparent;
}

IRAM_ATTR void draw_background() {
    for (int i = 0; i < kW * kH; ++i) s_work[i] = A::kBackground[i];
}

#ifndef CONFIG_RADIO_OCTOPUS_QUALITY_OFF
// Three bright wavering bands under the surface. Each brightens everything
// above its wavy lower edge, so overlapping bands stack to three levels.
void draw_caustics() {
    struct Band { float base, amp, freq, speed, phase, alpha; };
    static constexpr Band kBands[3] = {
        {5.0f, 2.6f, 0.055f, 0.018f, 0.0f, 0.170f},
        {12.0f, 2.1f, 0.077f, 0.029f, 2.1f, 0.125f},
        {19.0f, 1.6f, 0.099f, 0.040f, 4.2f, 0.080f},
    };
    const float rows0 = static_cast<float>(A::kSurfaceY) - 7.0f;  // surface offset into the band
    for (int b = 0; b < 3; ++b) {
        const Band &band = kBands[b];
        uint32_t ph = to_phase(s_clock * band.speed + band.phase);
        const uint32_t dph = to_phase(band.freq);
        const int base = static_cast<int>((band.base + rows0) * 256.0f) + 128;  // + 0.5: round
        const int amp = static_cast<int>(band.amp * 256.0f);
        const int a = static_cast<int>(band.alpha * 256.0f);
        const int ar = 190, ag = 250, ab = 255;
        for (int x = 0; x < kW; ++x, ph += dph) {
            const int rows = std::min(kH, (base + ((isin(ph) * amp) >> 14)) >> 8);
            for (int y = 0; y < rows; ++y) {
                uint8_t &px = s_work[y * kW + x];
                int r, g, bl;
                unpack(s_pal565[px], r, g, bl);
                r += ((ar - r) * a) >> 8;
                g += ((ag - g) * a) >> 8;
                bl += ((ab - bl) * a) >> 8;
                px = quantize(pack(r, g, bl));
            }
        }
    }
}

// Nine beams fan out from a point above the head. The source wanders, and every
// beam sways, pulses and changes width on its own period, as in the design.
// A beam is a thin wedge, brightest at the source and fading along its axis,
// added to what is already there. Per-beam values are worked out in float
// once; each row of each beam is then integer.
IRAM_ATTR void draw_rays() {
    const float src_x = A::kRaySourceX + fsin(s_clock * 0.013f) * 5.0f + fsin(s_clock * 0.031f) * 2.0f;
    const float src_y = A::kRaySourceY + fcos(s_clock * 0.019f) * 2.2f;
    const int32_t sx = static_cast<int32_t>(src_x * 256.0f);
    const int32_t sy = static_cast<int32_t>(src_y * 256.0f);

    // Soft bloom around the source
    const float bloom_r = 15.0f + fsin(s_clock * 0.045f) * 2.5f;
    const uint32_t inv_b2 = inv_radius2(bloom_r);
    const int by0 = std::max(0, static_cast<int>(src_y - bloom_r));
    const int by1 = std::min(kH - 1, static_cast<int>(src_y + bloom_r));
    const int bx0 = std::max(0, static_cast<int>(src_x - bloom_r));
    const int bx1 = std::min(kW - 1, static_cast<int>(src_x + bloom_r));
    for (int y = by0; y <= by1; ++y) {
        const int32_t dy = y * 256 + 128 - sy;
        uint8_t *row = s_work + y * kW;
        for (int x = bx0; x <= bx1; ++x) {
            const int32_t dx = x * 256 + 128 - sx;
            const int k = radial_index(dx * dx + dy * dy, inv_b2);
            if (k > 256) continue;
            const int a = s_bloom_level[k];
            if (a) add(row[x], (215 * a) >> 8, (255 * a) >> 8, (255 * a) >> 8);
        }
    }

    const int inv_reach = static_cast<int>(65536.0f / A::kBeamReach);  // Q16
    const int n = static_cast<int>(sizeof(A::kBeams) / sizeof(A::kBeams[0]));
    for (int i = 0; i < n; ++i) {
        const A::BeamDef &bm = A::kBeams[i];
        const float sway = fsin(s_clock * bm.wobble + i * 1.37f) * 0.055f;
        const float pulse = 0.72f + 0.28f * fsin(s_clock * bm.wobble * 1.6f + i * 2.11f);
        const float half = bm.half_width * (1.0f + fsin(s_clock * bm.wobble * 0.7f + i) * 0.12f);
        const float angle = kHalfPi + bm.angle + sway;
        const float dir_x = fcos(angle), dir_y = fsin(angle);
        if (dir_y < 0.2f) continue;
        const float inv_dir_y = 1.0f / dir_y;
        // The wedge's half-span on a row, per row below the source: the
        // perpendicular half-width stretched by 1 / sin for a tilted beam.
        // tan(half) is taken from its series; half is under 0.1 rad.
        const float tan_half = half * (1.0f + half * half * (1.0f / 3.0f));
        const int span_q12 = static_cast<int>(tan_half * inv_dir_y * inv_dir_y * 4096.0f);
        const int slope_q8 = static_cast<int>(dir_x * inv_dir_y * 256.0f);
        const int inv_dir_q8 = static_cast<int>(inv_dir_y * 256.0f);
        const int strength = static_cast<int>(bm.strength * pulse * 256.0f);

        for (int y = std::max(0, floor8(sy)); y < kH; ++y) {
            const int32_t dy = y * 256 + 128 - sy;      // Q8, rows below the source
            if (dy <= 0) continue;
            const int32_t t = (dy * inv_dir_q8) >> 8;     // distance along the axis, Q8
            if (t < 640) continue;                        // 2.5 px: leave the throat to the bloom
            const int32_t f = (t * inv_reach) >> 16;      // 0 at the source, 256 at the end, Q8
            if (f >= 256) break;
            const int a = (strength * s_beam_level[f]) >> 8;
            if (a <= 0) continue;
            const int32_t cx = sx + ((slope_q8 * dy) >> 8);
            const int32_t hw = std::max<int32_t>(154, (span_q12 * dy) >> 12);  // at least 0.6 px
            const int x0 = std::max(0, floor8(cx - hw));
            const int x1 = std::min(kW - 1, ceil8(cx + hw));
            const int ar = (200 * a) >> 8, ag = (245 * a) >> 8, ab = (255 * a) >> 8;
            uint8_t *row = s_work + y * kW;
            for (int x = x0; x <= x1; ++x) add(row[x], ar, ag, ab);
        }
    }
}

void draw_particles() {
    constexpr float span = kH + 20;
    for (int i = 0; i < 26; ++i) {
        const float seed = i * 137.5f;
        const float px = std::fmod(seed * 7.3f, static_cast<float>(kW));
        const float drift = std::fmod(s_clock * (0.09f + (i % 5) * 0.025f) + seed, span);
        const float py = kH + 10 - drift;
        blend_f(px, py, 0xd2f5ff, 0.10f + (i % 4) * 0.055f);
    }
}

float surface_at(float x) {
    return A::kSurfaceY + fsin(x * 0.09f + s_clock * 0.026f) * 1.1f +
           fsin(x * 0.21f - s_clock * 0.041f) * 0.5f;
}

void ellipse_outline(float cx, float cy, float rx, float ry, uint32_t rgb, float alpha,
                     bool upper_half_only) {
    const int a = alpha8(alpha);
    if (!a) return;
    const int steps = std::max(12, static_cast<int>(rx * 6.0f));
    const float from = upper_half_only ? 3.14159265f : 0.0f;
    const float span = upper_half_only ? 3.14159265f : kTwoPi;
    int last_x = -1000, last_y = -1000;
    for (int s = 0; s <= steps; ++s) {
        const float t = from + span * s / steps;
        const int x = static_cast<int>(std::floor(cx + fcos(t) * rx));
        const int y = static_cast<int>(std::floor(cy + fsin(t) * ry));
        if (x == last_x && y == last_y) continue;
        last_x = x;
        last_y = y;
        blend(x, y, rgb, a);
    }
}

// Radial glow fading linearly from `alpha` at the centre to 0 at `radius`.
void radial_glow(float cx, float cy, float radius, uint32_t rgb, float alpha) {
    const int a0 = alpha8(alpha);
    if (!a0 || radius <= 0.0f) return;
    const uint32_t inv_r2 = inv_radius2(radius);
    const int32_t qx = static_cast<int32_t>(cx * 256.0f), qy = static_cast<int32_t>(cy * 256.0f);
    const int x0 = static_cast<int>(std::floor(cx - radius)), x1 = static_cast<int>(std::ceil(cx + radius));
    const int y0 = static_cast<int>(std::floor(cy - radius)), y1 = static_cast<int>(std::ceil(cy + radius));
    for (int y = y0; y <= y1; ++y) {
        const int32_t dy = y * 256 + 128 - qy;
        for (int x = x0; x <= x1; ++x) {
            const int32_t dx = x * 256 + 128 - qx;
            const int k = radial_index(dx * dx + dy * dy, inv_r2);
            if (k < 256) blend(x, y, rgb, (a0 * s_glow_level[k]) >> 8);
        }
    }
}

// A bubble: a thin membrane ring, a faint fill and a highlight. The ring test
// compares the squared normalised distance against the squared ring bounds,
// so there is no square root per pixel.
void draw_bubble(float bx, float by, float rx, float ry, float a) {
    const float rmin = std::min(rx, ry);
    const float lw = std::max(0.7f, rmin * 0.26f);
    const float w = (lw * 0.5f + 0.35f) / rmin;
    const float lo = std::max(0.0f, 1.0f - w), hi = 1.0f + w;
    const int64_t lo2 = static_cast<int64_t>(lo * lo * 65536.0f);
    const int64_t hi2 = static_cast<int64_t>(hi * hi * 65536.0f);
    const int64_t kx = static_cast<int64_t>(65536.0f / (rx * rx));  // Q16 / Q16 px^2
    const int64_t ky = static_cast<int64_t>(65536.0f / (ry * ry));
    const bool fill = rmin > 1.3f;
    const int a_ring = alpha8(a), a_fill = alpha8(a * 0.18f);
    const int32_t qx = static_cast<int32_t>(bx * 256.0f), qy = static_cast<int32_t>(by * 256.0f);
    const int x0 = static_cast<int>(std::floor(bx - rx - 1)), x1 = static_cast<int>(std::ceil(bx + rx + 1));
    const int y0 = static_cast<int>(std::floor(by - ry - 1)), y1 = static_cast<int>(std::ceil(by + ry + 1));
    for (int y = y0; y <= y1; ++y) {
        const int64_t dy = y * 256 + 128 - qy;
        const int64_t ey = (dy * dy * ky) >> 16;
        for (int x = x0; x <= x1; ++x) {
            const int64_t dx = x * 256 + 128 - qx;
            const int64_t e = ((dx * dx * kx) >> 16) + ey;  // (d / r)^2 in Q16
            if (e >= lo2 && e <= hi2) {
                blend(x, y, 0xc8f5ff, a_ring);
            } else if (fill && e < lo2) {
                blend(x, y, 0xebffff, a_fill);
            }
        }
    }
    blend_f(bx - rx * 0.36f, by - ry * 0.40f, 0xf5ffff, a * 0.9f);
}

void draw_bubbles_and_surface() {
    const float travel = kBubbleStartY - A::kSurfaceY;
    for (const Bubble &b : kBubbles) {
        const float rise = travel / (b.sp * 1.45f);
        const float cycle = rise + kBurstFrames + 14;
        const float t = std::fmod(s_clock + b.sd * 3.1f, cycle);
        const float ph = s_clock * b.hf + b.sd * 0.3f;
        const float depth = fcos(ph);

        if (t < rise) {
            const float raw = t / rise;
            // Monotonic climb that slows as it meets the surface.
            const float prog = 1.0f - std::pow(1.0f - raw, 1.0f + raw * 0.55f);
            const float bx = b.x + fsin(ph) * b.hx * (1.0f - raw * 0.55f);
            const float surf = surface_at(bx);
            const float by = std::max(kBubbleStartY - prog * travel, surf + 0.6f);
            const float r = b.r * (0.74f + raw * 0.55f) * (0.88f + depth * 0.12f);
            const float press = raw > 0.93f ? (raw - 0.93f) / 0.07f : 0.0f;
            const float a = (0.28f + 0.24f * raw) * std::min(1.0f, raw * 6.0f) *
                            (0.82f + depth * 0.18f);
            draw_bubble(bx, by, r * (1.0f + press * 0.30f), r * (0.88f - press * 0.34f), a);

            for (int k = 1; k <= 2; ++k) {
                const float tr = prog - k * 0.045f;
                if (tr <= 0.02f) continue;
                const float wy = kBubbleStartY - tr * travel;
                if (wy < surf + 2) continue;
                const float wr = r * (0.26f - k * 0.07f);
                if (wr < 0.35f) continue;
                const float wx = b.x + fsin(ph - k * 0.5f) * b.hx * 0.7f * (1.0f - raw * 0.55f);
                blend_f(wx, wy, 0xd7faff, a * (0.40f - k * 0.12f));
            }
        } else if (t < rise + kBurstFrames) {
            const float u = (t - rise) / kBurstFrames;
            const float bx = b.x + fsin(ph) * b.hx * 0.45f;
            const float surf = surface_at(bx);
            const float r0 = b.r * 1.29f;

            // The film splits into a ring that opens outward
            const float ring = r0 * (1.0f + u * 1.35f);
            ellipse_outline(bx, surf + 0.5f, ring, ring * 0.30f, 0xe1fcff, (1.0f - u) * 0.62f, true);
            if (u < 0.28f) radial_glow(bx, surf, r0 * 1.6f, 0xffffff, (1.0f - u / 0.28f) * 0.55f);

            // Droplets thrown up on short arcs
            const float tt = u * kBurstFrames;
            for (int k = 0; k < 5; ++k) {
                const int seed = static_cast<int>(b.sd);
                const float dir = (k - 2) * 0.55f + (((seed * 7 + k * 13) % 10) - 5) * 0.03f;
                const float vy = 3.0f + ((seed + k * 5) % 4) * 0.55f;
                const float px = bx + dir * 3.4f * tt * 0.22f;
                const float py = surf - (vy * tt * 0.22f - 0.5f * 0.42f * tt * tt * 0.05f);
                if (py > surf) continue;
                blend_f(px, py, 0xebffff, (1.0f - u) * 0.8f);
            }

            // Ripples spreading along the surface
            for (int k = 0; k < 2; ++k) {
                const float uu = std::max(0.0f, u - k * 0.18f);
                if (uu <= 0.0f) continue;
                const float rr = r0 * (1.1f + uu * 3.6f);
                ellipse_outline(bx, surf, rr, rr * 0.16f, 0xd2faff, (1.0f - uu) * 0.38f, false);
            }
        }
    }

    // Waterline: bright edge with a dimmer band just below. Same curve as
    // surface_at(), stepped in fixed point across the row.
    uint32_t p1 = to_phase(0.5f * 0.09f + s_clock * 0.026f);
    uint32_t p2 = to_phase(0.5f * 0.21f - s_clock * 0.041f);
    const uint32_t d1 = to_phase(0.09f), d2 = to_phase(0.21f);
    const int base = static_cast<int>(A::kSurfaceY * 256.0f);
    const int a_edge = alpha8(0.55f), a_band = alpha8(0.22f);
    for (int x = 0; x < kW; ++x, p1 += d1, p2 += d2) {
        const int y = (base + ((isin(p1) * 282) >> 14) + ((isin(p2) * 128) >> 14)) >> 8;  // 1.1, 0.5 px
        blend(x, y, 0xd7fcff, a_edge);
        blend(x, y + 1, 0xaaf0ff, a_band);
        blend(x, y + 2, 0xaaf0ff, a_band);
    }
}

void draw_arm(int index) {
    const A::ArmDef &arm = A::kArms[index];
    const A::ArmAnim &anim = A::kArmAnim[index];
    const uint16_t *colors = s_arm_color[index];

    // Rest pose plus a vertical wave that travels root -> tip.
    const float amp = (2.6f + (7.0f - 2.6f) * s_mix) * anim.amp;
    int32_t cx[10], cy[10];
    for (int j = 0; j < 10; ++j) {
        const float w = A::kSway[j];
        const float ph = s_phase[index] - w * 2.0f;
        const float lift = fsin(ph) * amp * w * 1.15f;
        const float reach = fsin(ph - kHalfPi) * amp * w * 0.30f;
        cx[j] = static_cast<int32_t>((arm.pts[j][0] + reach * arm.side) * 256.0f);
        cy[j] = static_cast<int32_t>((arm.pts[j][1] - lift) * 256.0f);
    }

    // Catmull-Rom through the control points
    int n = 0;
    for (int s = 0; s < kSegments; ++s) {
        const int i0 = std::max(0, s - 1), i1 = s, i2 = s + 1, i3 = std::min(9, s + 2);
        const int last = s == kSegments - 1 ? kSamplesPerSegment : kSamplesPerSegment - 1;
        for (int k = 0; k <= last; ++k) {
            const int16_t *w = s_basis[k];
            s_px[n] = (w[0] * cx[i0] + w[1] * cx[i1] + w[2] * cx[i2] + w[3] * cx[i3]) >> 12;
            s_py[n] = (w[0] * cy[i0] + w[1] * cy[i1] + w[2] * cy[i2] + w[3] * cy[i3]) >> 12;
            ++n;
        }
    }

    // Normals (pointing up the screen, so highlights sit on the top edge)
    // and segment lengths
    uint32_t total = 0;
    s_seg[0] = 0;
    for (int i = 0; i < n; ++i) {
        const int a = std::max(0, i - 1), b = std::min(n - 1, i + 1);
        const int32_t dx = s_px[b] - s_px[a], dy = s_py[b] - s_py[a];
        int32_t len = static_cast<int32_t>(isqrt(static_cast<uint32_t>(dx * dx + dy * dy)));
        if (!len) len = 1;
        s_nx[i] = static_cast<int16_t>(-dy * 256 / len);
        s_ny[i] = static_cast<int16_t>(dx * 256 / len);
        if (i) {
            const int32_t sx = s_px[i] - s_px[i - 1], sy = s_py[i] - s_py[i - 1];
            s_seg[i] = static_cast<uint16_t>(isqrt(static_cast<uint32_t>(sx * sx + sy * sy)));
            total += s_seg[i];
        }
    }
    if (!total) total = 1;

    const int32_t root = static_cast<int32_t>(arm.root_width * 256.0f);
    const int32_t tip = static_cast<int32_t>(arm.tip_width * 256.0f);
    auto half_width = [&](int i) { return (tip + (((root - tip) * s_taper[i]) >> 12)) / 2; };

    // Volume shading: dark rim, then bands that stack toward the top edge.
    static constexpr int kShrink[6] = {0, 26, 67, 118, 169, 215};  // /256
    static constexpr int kBias[6] = {0, 0, 26, 46, 61, 72};        // /256
    // Lite draws the rim, the base and the highlight only, and spaces the
    // stamps further apart. Each stamp is a filled disc, so this removes most
    // of the overdraw while the arm keeps its volume.
    static constexpr int kLiteBands[3] = {0, 2, 4};
    constexpr int kBandCount = kLite ? 3 : 6;
    for (int band = 0; band < kBandCount; ++band) {
        const int layer = kLite ? kLiteBands[band] : band;
        const uint16_t color = colors[layer];
        int32_t acc = 1 << 30;
        for (int i = 0; i < n; ++i) {
            acc += s_seg[i];
            const int32_t wf = half_width(i);
            const int32_t r = (wf * (256 - kShrink[layer])) >> 8;
            const int32_t step = kLite ? std::max<int32_t>(256, (r * 230) >> 8)
                                       : std::max<int32_t>(154, (r * 115) >> 8);
            if (acc < step && i != n - 1) continue;
            acc = 0;
            int32_t ux = s_nx[i], uy = s_ny[i];
            if (uy > 0) {
                ux = -ux;
                uy = -uy;
            }
            const int32_t shift = (wf * kBias[layer]) >> 8;
            const int32_t px = s_px[i] + ((ux * shift) >> 8);
            const int32_t py = s_py[i] + ((uy * shift) >> 8);
            disc(px, py, r, color);
            if (layer == 0 && r < 160) plot(floor8(px), floor8(py), color);
        }
    }

    // Suckers: few and large near the root, none past ~74% of the length.
    uint32_t travelled = 0;
    int32_t acc = 0;
    for (int i = 1; i < n; ++i) {
        travelled += s_seg[i];
        acc += s_seg[i];
        const uint32_t t12 = travelled * 4096u / total;
        if (t12 < 410 || t12 > 3031) continue;
        const int32_t arm_w = half_width(static_cast<int>(t12 * (kArmSamples - 1) / 4096));
        const int32_t sr = (arm_w * 133) >> 8;
        if (sr < 269) continue;
        if (acc < ((sr * 794) >> 8)) continue;
        acc = 0;
        const int32_t off = ((arm_w * 77) >> 8) * arm.side;
        const int32_t sx = s_px[i] + ((s_nx[i] * off) >> 8);
        const int32_t sy = s_py[i] + ((s_ny[i] * off) >> 8);
        disc(sx, sy, (sr * 312) >> 8, colors[0]);
        disc(sx, sy, sr, colors[6]);
        disc(sx + ((sr * 26) >> 8), sy + ((sr * 31) >> 8), (sr * 133) >> 8, colors[7]);
        disc(sx + ((sr * 36) >> 8), sy + ((sr * 41) >> 8), (sr * 67) >> 8, colors[8]);
        if (sr > 384) disc(sx - ((sr * 97) >> 8), sy - ((sr * 102) >> 8), (sr * 51) >> 8, 0xffff);
    }
}

IRAM_ATTR void draw_body() {
    for (int y = s_body_y0; y <= s_body_y1; ++y) {
        for (int x = s_body_x0; x <= s_body_x1; ++x) {
            const uint8_t v = A::kBody[y * kW + x];
            if (v != A::kTransparent) s_work[y * kW + x] = v;
        }
    }
}

// Gold rim light on the crest of the mantle, following the overhead source.
// Two quadratic arcs as in the design, a wide gold one and a narrow bright
// core, kept to pixels the body actually covers.
void draw_crest_glint() {
    const float drift = fsin(s_clock * 0.013f) * 5.0f + fsin(s_clock * 0.031f) * 2.0f;
    const float shift = drift * 0.5f;
    const float alpha = 0.48f + 0.16f * fsin(s_clock * 0.045f);
    const float cx = A::kMantleX, my = A::kMantleY;
    const float mw = A::kMantleW, mh = A::kMantleH;

    auto arc = [&](float x0, float y0, float cx1, float cy1, float x1, float y1, float width,
                   uint32_t rgb, float a) {
        constexpr int kSteps = 40;
        const int a8 = alpha8(a);
        for (int i = 0; i <= kSteps; ++i) {
            const float t = static_cast<float>(i) / kSteps, u = 1.0f - t;
            const float px = u * u * x0 + 2 * u * t * cx1 + t * t * x1;
            const float py = u * u * y0 + 2 * u * t * cy1 + t * t * y1;
            const int ix = static_cast<int>(std::floor(px)), iy = static_cast<int>(std::floor(py));
            for (int dy = 0; dy < static_cast<int>(std::ceil(width)); ++dy) {
                const int y = iy + dy;
                if (x_in_body(ix, y)) blend(ix, y, rgb, a8);
            }
        }
    };
    arc(cx - mw * 0.52f + shift, my - mh * 0.34f, cx + shift, my - mh * 0.63f,
        cx + mw * 0.52f + shift, my - mh * 0.34f, 2.4f, A::kRgb_gA, alpha);
    arc(cx - mw * 0.30f + shift, my - mh * 0.45f, cx + shift, my - mh * 0.60f,
        cx + mw * 0.30f + shift, my - mh * 0.45f, 1.1f, 0xfffae1, alpha * 0.7f);
}

void draw_sparkle() {
    const float idle = 0.55f + 0.2f * std::fabs(fsin(s_clock * 0.035f));
    const float play = 0.6f + 0.4f * std::fabs(fsin(s_clock * 0.14f));
    const float base = idle + (play - idle) * s_mix;
    const float x = A::kSparkleX, y = A::kSparkleY;

    radial_glow(x, y, 5.0f * base, 0xffffff, 0.6f * base);
    const uint16_t white = 0xffff;
    const int ix = static_cast<int>(std::floor(x)), iy = static_cast<int>(std::floor(y));
    const int arm = static_cast<int>(std::lround(3.6f * base));
    for (int d = -arm; d <= arm; ++d) {
        plot(ix + d, iy, white);
        plot(ix, iy + d, white);
    }
    const int diag = static_cast<int>(std::lround(3.6f * base * 0.42f));
    for (int d = -diag; d <= diag; ++d) {
        plot(ix + d, iy + d, white);
        plot(ix + d, iy - d, white);
    }
}
#endif  // !CONFIG_RADIO_OCTOPUS_QUALITY_OFF

// ── output ─────────────────────────────────────────────────

// Header band colours per palette entry, refreshed when the overlay's band
// changes; the band is a fixed translucent fill, so it can be applied to the
// palette instead of to every pixel.
uint32_t s_band_key = 0xffffffffu;
void refresh_band_palette() {
    const uint32_t key = s_overlay ? (s_overlay->band_rgb << 8) ^ s_overlay->band_alpha : 0;
    if (key == s_band_key) return;
    s_band_key = key;
    const int a = s_overlay ? s_overlay->band_alpha + (s_overlay->band_alpha >> 7) : 0;  // 0..256
    for (int i = 0; i < A::kPaletteSize; ++i) {
        s_pal_band[i] = swap16(s_overlay ? mix(s_pal565[i], s_overlay->band_rgb, a) : s_pal565[i]);
    }
}

// Composite one panel row of the overlay onto `line` (panel byte order):
// the rule, then any text coverage.
IRAM_ATTR void overlay_row(int py, uint16_t *line) {
    const OctopusOverlay *ov = s_overlay;
    if (!ov || py >= kOverlayRows) return;
    if (py == ov->rule_y) {
        const int a = ov->rule_alpha + (ov->rule_alpha >> 7);
        for (int x = std::max<int>(0, ov->rule_x0); x < std::min<int>(kPanelWidth, ov->rule_x1); ++x) {
            line[x] = swap16(mix(swap16(line[x]), ov->rule_rgb, a));
        }
    }
    const uint8_t *cov = ov->coverage + py * kOverlayStride;
    for (int bx = 0; bx < kOverlayStride; ++bx) {
        const uint8_t pair = cov[bx];
        if (!pair) continue;
        for (int half = 0; half < 2; ++half) {
            const int c = half ? (pair & 15) : (pair >> 4);
            if (!c) continue;
            const int x = bx * 2 + half;
            uint32_t rgb = 0xffffff;
            for (int k = 0; k < ov->ink_count; ++k) {
                const OctopusInk &ink = ov->inks[k];
                if (x >= ink.x0 && x < ink.x1 && py >= ink.y0 && py < ink.y1) {
                    rgb = ink.rgb;
                    break;
                }
            }
            line[x] = swap16(mix(swap16(line[x]), rgb, c * 17 + (c >> 3)));
        }
    }
}

// One composited panel row in panel byte order, from quantized scene indices.
IRAM_ATTR void compose_row(int py, const uint8_t *idx, uint16_t *line) {
    const OctopusOverlay *ov = s_overlay;
    const bool in_band = ov && py < ov->band_rows;
    const int fade = ov && !in_band ? ov->band_rows + ov->band_fade - py : 0;
    if (fade > 0) {
        // Below the band its alpha steps down to zero, so it has no edge.
        const int band_a = ov->band_alpha + (ov->band_alpha >> 7);
        const int a = band_a * fade / (ov->band_fade + 1);
        for (int sx = 0; sx < kW; ++sx) {
            const uint16_t v = swap16(mix(s_pal565[idx[sx]], ov->band_rgb, a));
            line[sx * 2] = v;
            line[sx * 2 + 1] = v;
        }
    } else {
        const uint16_t *pal = in_band ? s_pal_band : s_pal_panel;
        for (int sx = 0; sx < kW; ++sx) {
            const uint16_t v = pal[idx[sx]];
            line[sx * 2] = v;
            line[sx * 2 + 1] = v;
        }
    }
    overlay_row(py, line);
}

void drain() {
    if (!s_out_pending) return;
    // A parameter command waits for every queued colour transfer, so the
    // stripe is free to overwrite afterwards. ST7789 0x00 is NOP.
    if (bsp_display_io()) esp_lcd_panel_io_tx_param(bsp_display_io(), 0x00, nullptr, 0);
    s_out_pending = false;
}

bool send_frame() {
    esp_lcd_panel_handle_t panel = bsp_display_panel();
    if (!panel) return false;
    refresh_band_palette();

    for (int y = 0; y < kH; y += kChunkRows) {
        const int y_end = std::min(y + kChunkRows, kH);
        drain();
        uint16_t *dst = s_out;
        for (int row = y; row < y_end; ++row) {
            const uint8_t *src = s_work + row * kW;
            const int py = kOctopusPanelTop + row * 2;
            const bool overlay = s_overlay && py < kOverlayRows;
            compose_row(py, src, dst);
            if (overlay) {
                compose_row(py + 1, src, dst + kPanelWidth);
            } else {
                std::memcpy(dst + kPanelWidth, dst, kPanelWidth * sizeof(uint16_t));
            }
            dst += kPanelWidth * 2;
        }
        if (esp_lcd_panel_draw_bitmap(panel, 0, kOctopusPanelTop + y * 2, kPanelWidth,
                                      kOctopusPanelTop + y_end * 2, s_out) != ESP_OK) {
            return false;
        }
        s_out_pending = true;
    }
    // Finish before returning. The panel driver reports every finished colour
    // transfer to LVGL as "flush ready"; one of ours completing while LVGL is
    // mid-flush would let it reuse its draw buffer too early.
    drain();
    return true;
}

bool ensure_buffers() {
    if (!s_tables_ready) init_tables();
    if (s_work && s_out && s_overlay) return true;
    const int64_t now = esp_timer_get_time();
    if (now < s_retry_after_us) return false;

    if (!s_work) {
        s_work = static_cast<uint8_t *>(
            heap_caps_malloc(kW * kH * sizeof(uint8_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    if (!s_out) {
        s_out = static_cast<uint16_t *>(heap_caps_malloc(
            kPanelWidth * kChunkRows * 2 * sizeof(uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    }
    if (!s_overlay) {
        s_overlay = static_cast<OctopusOverlay *>(
            heap_caps_calloc(1, sizeof(OctopusOverlay), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        if (s_overlay) s_overlay->rule_y = -1;
    }
    if (!s_work || !s_out || !s_overlay) {
        ESP_LOGW(kTag, "Scene buffers unavailable (free %u, largest %u); retrying later",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
        radio_octopus_release();
        s_retry_after_us = now + 5 * 1000 * 1000;
        return false;
    }
    s_last_us = 0;
    s_band_key = 0xffffffffu;
    ++s_epoch;
    ESP_LOGI(kTag, "Scene buffers ready (variant %s, frame %d ms), internal heap free %u",
             RADIO_OCTOPUS_VARIANT_NAME, kOctopusFrameMs,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
    return true;
}

void advance_time(bool playing) {
    const int64_t now = esp_timer_get_time();
    float dt_ms = s_last_us ? static_cast<float>(now - s_last_us) / 1000.0f : 0.0f;
    dt_ms = std::min(dt_ms, 200.0f);
    s_last_us = now;

    const float frames = dt_ms * 0.018f;
    s_clock += frames;
    // Long-running float time loses precision; wrap it about every 90 min.
    if (s_clock > 100000.0f) s_clock -= 100000.0f;

    const float target = playing ? 1.0f : 0.0f;
    if (!s_mix_pinned) s_mix += (target - s_mix) * std::min(1.0f, dt_ms / 700.0f);

    for (int i = 0; i < 8; ++i) {
        const float speed = (0.020f + (0.072f - 0.020f) * s_mix) * A::kArmAnim[i].freq;
        s_phase[i] = std::fmod(s_phase[i] + speed * frames, kTwoPi);
    }
}

}  // namespace

OctopusOverlay *radio_octopus_acquire() { return ensure_buffers() ? s_overlay : nullptr; }

uint32_t radio_octopus_epoch() { return s_epoch; }

uint32_t radio_octopus_last_frame_us() { return s_last_frame_us; }

bool radio_octopus_active() { return s_frame_held; }

void radio_octopus_pause() { s_last_us = 0; }

bool radio_octopus_draw_frame(bool playing) {
    if (!ensure_buffers()) return false;
    const int64_t start = esp_timer_get_time();
    int64_t mark = start;
    auto phase = [&](Phase p) {
        const int64_t now = esp_timer_get_time();
        s_phase_us[p] += now - mark;
        mark = now;
    };

    advance_time(playing);
    draw_background();
    phase(kPhaseBackground);
#ifndef CONFIG_RADIO_OCTOPUS_QUALITY_OFF
    draw_caustics();
    phase(kPhaseCaustics);
    draw_rays();
    phase(kPhaseRays);
    draw_particles();
    phase(kPhaseParticles);
    draw_bubbles_and_surface();
    phase(kPhaseBubbles);
    for (int i : kDrawOrder) draw_arm(i);
    phase(kPhaseArms);
    draw_body();
    phase(kPhaseBody);
    draw_crest_glint();
    phase(kPhaseGlint);
    draw_sparkle();
    phase(kPhaseSparkle);
#endif
    s_frame_held = true;
    const bool ok = send_frame();
    phase(kPhaseSend);

    const int64_t frame_us = esp_timer_get_time() - start;
    s_last_frame_us = static_cast<uint32_t>(frame_us);
    s_stat_us += frame_us;
    s_stat_max_us = std::max(s_stat_max_us, frame_us);
    if (++s_stat_frames == 150) {
        const auto avg = [&](Phase p) { return static_cast<unsigned>(s_phase_us[p] / s_stat_frames); };
        ESP_LOGD(kTag, "[%s] avg frame %u us (max %u), internal heap free %u",
                 RADIO_OCTOPUS_VARIANT_NAME,
                 static_cast<unsigned>(s_stat_us / s_stat_frames),
                 static_cast<unsigned>(s_stat_max_us),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
        ESP_LOGD(kTag, "phase us: bg %u caustics %u rays %u particles %u bubbles %u arms %u "
                 "body %u glint %u sparkle %u send %u",
                 avg(kPhaseBackground), avg(kPhaseCaustics), avg(kPhaseRays), avg(kPhaseParticles),
                 avg(kPhaseBubbles), avg(kPhaseArms), avg(kPhaseBody), avg(kPhaseGlint),
                 avg(kPhaseSparkle), avg(kPhaseSend));
        s_stat_frames = 0;
        s_stat_us = 0;
        s_stat_max_us = 0;
        for (int64_t &v : s_phase_us) v = 0;
    }
    return ok;
}

void radio_octopus_release() {
    drain();
    heap_caps_free(s_work);
    s_work = nullptr;
    heap_caps_free(s_out);
    s_out = nullptr;
    heap_caps_free(s_overlay);
    s_overlay = nullptr;
    s_frame_held = false;
}

extern "C" bool radio_octopus_capture_row(int y, uint16_t *row) {
    if (!s_work || !s_frame_held || y < 0 || y >= kOctopusPanelHeight) return false;
    refresh_band_palette();
    const uint8_t *src = s_work + ((y - kOctopusPanelTop) / 2) * kW;
    compose_row(y, src, row);
    for (int x = 0; x < kPanelWidth; ++x) row[x] = swap16(row[x]);
    return true;
}

#ifdef RADIO_OCTOPUS_TESTING
void radio_octopus_test_set_mix(float mix) {
    s_mix = mix;
    s_mix_pinned = true;
}

void radio_octopus_test_reset(float mix) {
    s_clock = 0;
    s_last_us = 0;
    for (int i = 0; i < 8; ++i) s_phase[i] = A::kArmAnim[i].phase;
    s_mix = mix;
    s_mix_pinned = true;
}
#endif
