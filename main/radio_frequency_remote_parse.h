#pragma once

#include <cstdint>

using RadioFrequencyParser = uint16_t (*)(const char *text);

// These helpers contain no ESP-IDF dependencies so their input handling is
// covered by host tests.
bool radio_frequency_qingting_channel_id(const char *url,
                                         uint32_t *channel_id);
uint16_t radio_frequency_parse_metadata(const char *response,
                                        RadioFrequencyParser parser);
