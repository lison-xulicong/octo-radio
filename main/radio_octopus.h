#pragma once

#include <cstdint>

// Animated pixel octopus for the main page. The scene is a 120x95 image drawn
// at 2x straight to the panel, bypassing LVGL's flush, over
// the top 190 rows of the screen, where LVGL draws nothing. The header and the
// channel label sit on top of the scene, so they are composited here from an
// overlay that radio_ui paints with LVGL's fonts. Call only from the LVGL task,
// or with the LVGL lock held.
#if __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

// Build-time variant, set in menuconfig under "OCTO Radio". Full is the
// default; Lite shades the arms with fewer bands; Off draws water only.
#if defined(CONFIG_RADIO_OCTOPUS_QUALITY_OFF)
#define RADIO_OCTOPUS_VARIANT_NAME "off"
#elif defined(CONFIG_RADIO_OCTOPUS_QUALITY_LITE) || defined(RADIO_OCTOPUS_LITE)
#define RADIO_OCTOPUS_VARIANT_NAME "lite"
#else
#define RADIO_OCTOPUS_VARIANT_NAME "full"
#endif

#ifdef CONFIG_RADIO_OCTOPUS_FRAME_MS
constexpr int kOctopusFrameMs = CONFIG_RADIO_OCTOPUS_FRAME_MS;
#else
constexpr int kOctopusFrameMs = 80;
#endif

constexpr int kOctopusPanelTop = 0;
constexpr int kOctopusPanelHeight = 190;

// Overlay at panel resolution over the top kOverlayRows rows. Coverage is
// 4-bit, two pixels per byte with the even x in the high nibble. A covered
// pixel takes the colour of the first ink rectangle that contains it.
constexpr int kOverlayRows = 62;
constexpr int kOverlayStride = 120;
constexpr int kOverlayInks = 12;

struct OctopusInk {
    int16_t x0, y0, x1, y1;  // right/bottom exclusive
    uint32_t rgb;
};

struct OctopusOverlay {
    uint8_t coverage[kOverlayRows * kOverlayStride];
    OctopusInk inks[kOverlayInks];
    uint8_t ink_count;
    // A translucent band behind the header, fading out over band_fade rows
    // below it, and an optional thin rule.
    uint8_t band_rows;
    uint8_t band_fade;
    uint8_t band_alpha;  // 0..255
    uint32_t band_rgb;
    int16_t rule_y, rule_x0, rule_x1;  // rule_x1 exclusive; rule_y < 0 for none
    uint8_t rule_alpha;
    uint32_t rule_rgb;
};

// Allocates the scene buffers if needed and returns the overlay to paint, or
// nullptr while memory is short. The overlay is cleared whenever the buffers
// are (re)allocated, and radio_octopus_epoch() changes when that happens.
OctopusOverlay *radio_octopus_acquire();
uint32_t radio_octopus_epoch();

// Renders and sends one frame. Returns false if the buffers are not available
// or the panel rejected a transfer.
bool radio_octopus_draw_frame(bool playing);

// Wall time of the last frame, for pacing the caller's timer.
uint32_t radio_octopus_last_frame_us();

// True while the scene is on the panel: buffers are held and a frame has been
// drawn since they were allocated.
bool radio_octopus_active();

// Stops advancing scene time without releasing the panel buffers. Used while
// the display is blanked so wake and station changes cannot expose black rows.
void radio_octopus_pause();

// Waits for in-flight transfers and frees the frame buffers.
void radio_octopus_release();

// Serial screenshot support: the last frame's panel row `y` (0-based, below
// kOctopusPanelHeight), composited with the overlay, as native-endian RGB565.
extern "C" bool radio_octopus_capture_row(int y, uint16_t *row);

#ifdef RADIO_OCTOPUS_TESTING
// Host tests pin the idle/playing blend so frames can be compared with the
// simulator, whose motion is a closed-form function of (clock, mode).
void radio_octopus_test_set_mix(float mix);

// Rewinds the animation to t = 0 with the blend pinned, so a frame can be
// reproduced at an exact simulator clock.
void radio_octopus_test_reset(float mix);
#endif
