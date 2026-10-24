#pragma once

#include "radio_octopus.h"

// The main page's header (brand, clock, signal, battery) and channel row,
// painted into the octopus overlay with LVGL's fonts. Kept apart from
// radio_ui so the host tools can render it exactly as the device does.

constexpr const char *kRadioBrand = "OCTO RADIO";

struct RadioHeaderText {
    char clock[8] = "--:--";
    char battery[8] = "--%";
    bool battery_low = false;
    bool network = false;
    char channel[20] = "CH 01 / 01";
    char sleep[24] = "";
};

void radio_header_paint(OctopusOverlay *ov, const RadioHeaderText &text,
                        const char *brand = kRadioBrand);
