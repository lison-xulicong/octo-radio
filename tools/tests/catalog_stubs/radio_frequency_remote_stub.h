#pragma once

#include "radio_player.h"

#include <cstddef>
#include <cstdint>

using RadioFrequencyParser = uint16_t (*)(const char *text);

inline void radio_frequency_enrich_online(RadioStation *, std::size_t,
                                          RadioFrequencyParser) {}
