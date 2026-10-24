#pragma once

#include <cstddef>
#include <cstdint>

// Looks up a city-specific FM frequency from the checked-in offline table.
// Returns false when the station is unknown or has no verified/declared entry.
bool radio_frequency_lookup(const char *city, const char *station,
                            uint16_t *frequency_decihz);

// Exposed for host tests and the table generator contract.
uint64_t radio_frequency_key(const char *city, const char *station);
