// Fake display for host tests of main/radio_octopus.cc. The renderer calls the
// panel through bsp_display.h and esp_lcd_*; octopus_fake_panel.cc implements
// those and records what was drawn so tests can inspect whole frames.
#pragma once

#include "bsp_display.h"
#include "esp_timer.h"
#include "radio_octopus.h"

#include <cstdint>

namespace fakelcd {
inline constexpr int kWidth = 240;
inline constexpr int kTop = kOctopusPanelTop;
inline constexpr int kHeight = kOctopusPanelHeight;
extern uint16_t framebuffer[kWidth * kHeight];
extern uint8_t written[kWidth * kHeight];
extern int bad_rects;
extern int draws;
}  // namespace fakelcd
