#pragma once

#include "radio_player.h"

#include <cstddef>
#include <cstdint>

struct RadioLocation {
    char country_code[4];
    char region[48];
    char city[48];
    // What the dial shows for this search: the located city, the chosen city or
    // the search text. Kept apart from city/region, which name the stations and
    // so follow where they came from, not where the network says the radio is.
    char label[48];
    // What the stations are called ("<place>电台 3") when their own names cannot
    // be shown. Follows where the stations came from, so a radio that located
    // itself abroad still names its stations after China.
    char place[48];
};

struct RadioCityMatch {
    const char *name;
    const char *province;
    uint8_t priority;
};

// The display font carries only a fixed set of Chinese glyphs. The catalog asks
// this before it puts Chinese text on screen, and falls back to the Latin name
// when a character is missing. Without one every text is taken as drawable.
using RadioRenderableFn = bool (*)(const char *text);
void radio_catalog_set_renderable(RadioRenderableFn check);

// Locates the radio from its public network address and loads playable MP3
// stations for the detected city. If the directory has no city result, it
// automatically falls back to the surrounding region/province.
std::size_t radio_catalog_discover(RadioStation *stations, std::size_t capacity,
                                   RadioLocation *location);

// Loads the most popular playable stations for a user-selected Chinese city.
// This skips IP geolocation and is used by the on-device city selector.
std::size_t radio_catalog_discover_city(const char *city, RadioStation *stations,
                                        std::size_t capacity,
                                        RadioLocation *location);

// Searches the directory for a uniquely matched city. The query may be Chinese,
// full pinyin, or initials. Ambiguous queries return 0 so the UI can ask the
// listener to choose a province before making a directory request.
std::size_t radio_catalog_search(const char *query, RadioStation *stations,
                                 std::size_t capacity, RadioLocation *location);

// Pinyin to Chinese, longest match first, for the words radio stations use.
// Writes the Chinese text and returns true only when the whole input was
// understood. Exposed for the host tests.
bool radio_catalog_pinyin_to_chinese(const char *pinyin, char *out, std::size_t capacity);
// Maps an unambiguous initials query such as "wx" to its Chinese city name.
bool radio_catalog_city_initials(const char *initials, char *out, std::size_t capacity);
// Returns every city matching Chinese name, full pinyin, or initials.
std::size_t radio_catalog_find_cities(const char *query, RadioCityMatch *matches,
                                      std::size_t capacity);
bool radio_catalog_unique_city(const char *query, char *out, std::size_t capacity);
