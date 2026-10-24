#pragma once

#include "radio_frequency_remote_parse.h"
#include "radio_player.h"

#include <cstddef>

// Resolves frequencies missing from the built-in compact table. Positive
// results are persisted by Qingting channel ID, so a station remains cached
// even if its display name changes. "No FM metadata" is remembered only for
// this run, allowing a later boot to pick up newly added metadata. A transport
// failure stops the batch to avoid one timeout per row.
void radio_frequency_enrich_online(RadioStation *stations, std::size_t count,
                                   RadioFrequencyParser parser);
