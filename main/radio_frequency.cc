#include "radio_frequency.h"

#include "radio_frequency_data.h"

#include <cctype>
#include <cstdint>
#include <cstring>

namespace {

constexpr uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;

bool punctuation(uint32_t codepoint) {
    switch (codepoint) {
        case 0x00b7:  // middle dot
        case 0x2013:  // en dash
        case 0x2014:  // em dash
        case 0x3001:  // ideographic comma
        case 0x3002:  // ideographic full stop
        case 0xff08:  // full-width left parenthesis
        case 0xff09:  // full-width right parenthesis
        case 0xff0c:  // full-width comma
        case 0xff1a:  // full-width colon
            return true;
        default:
            return false;
    }
}

std::size_t decode_utf8(const uint8_t *input, uint32_t *codepoint,
                        std::size_t *length) {
    const uint8_t first = input[0];
    if (first < 0x80) {
        *codepoint = first;
        *length = 1;
        return 1;
    }
    int extra = 0;
    uint32_t value = 0;
    if ((first & 0xe0) == 0xc0) {
        value = first & 0x1f;
        extra = 1;
    } else if ((first & 0xf0) == 0xe0) {
        value = first & 0x0f;
        extra = 2;
    } else if ((first & 0xf8) == 0xf0) {
        value = first & 0x07;
        extra = 3;
    } else {
        *codepoint = first;
        *length = 1;
        return 0;
    }
    for (int i = 1; i <= extra; ++i) {
        if ((input[i] & 0xc0) != 0x80) {
            *codepoint = first;
            *length = 1;
            return 0;
        }
        value = (value << 6) | (input[i] & 0x3f);
    }
    *codepoint = value;
    *length = static_cast<std::size_t>(extra + 1);
    return *length;
}

std::size_t normalize(const char *source, uint8_t *target,
                      std::size_t capacity) {
    if (!source || !target || capacity == 0) return 0;
    const auto *p = reinterpret_cast<const uint8_t *>(source);
    std::size_t used = 0;
    while (*p && used < capacity) {
        // FM/AM is metadata when followed by a number. Remove both the marker
        // and number so "新闻广播 AM1161 FM93.7" matches "新闻广播".
        if ((p[0] == 'F' || p[0] == 'f' || p[0] == 'A' || p[0] == 'a') &&
            (p[1] == 'M' || p[1] == 'm')) {
            const uint8_t *digits = p + 2;
            while (*digits == ' ') ++digits;
            if (*digits >= '0' && *digits <= '9') {
                p = digits;
                while ((*p >= '0' && *p <= '9') || *p == '.') ++p;
                continue;
            }
        }

        uint32_t codepoint = 0;
        std::size_t length = 0;
        decode_utf8(p, &codepoint, &length);
        if (codepoint < 0x80) {
            if (std::isalnum(codepoint) && used < capacity) {
                target[used++] =
                    static_cast<uint8_t>(std::tolower(codepoint));
            }
        } else if (!punctuation(codepoint)) {
            if (used + length > capacity) break;
            std::memcpy(target + used, p, length);
            used += length;
        }
        p += length ? length : 1;
    }
    return used;
}

uint64_t append_hash(uint64_t hash, const uint8_t *data, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= kFnvPrime;
    }
    return hash;
}

}  // namespace

uint64_t radio_frequency_key(const char *city, const char *station) {
    uint8_t city_text[96] = {};
    uint8_t station_text[192] = {};
    const std::size_t city_size = normalize(city, city_text, sizeof(city_text));
    const std::size_t station_size =
        normalize(station, station_text, sizeof(station_text));
    if (!city_size || !station_size) return 0;

    uint64_t hash = append_hash(kFnvOffset, city_text, city_size);
    hash ^= 0x1f;
    hash *= kFnvPrime;
    return append_hash(hash, station_text, station_size);
}

bool radio_frequency_lookup(const char *city, const char *station,
                            uint16_t *frequency_decihz) {
    if (!frequency_decihz) return false;
    *frequency_decihz = 0;
    const uint64_t key = radio_frequency_key(city, station);
    if (!key) return false;

    std::size_t low = 0;
    std::size_t high = kRadioFrequencyCount;
    while (low < high) {
        const std::size_t middle = low + (high - low) / 2;
        const uint64_t current = kRadioFrequencyKeys[middle];
        if (current < key) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low >= kRadioFrequencyCount || kRadioFrequencyKeys[low] != key) {
        return false;
    }
    *frequency_decihz = kRadioFrequencyValues[low];
    return *frequency_decihz != 0;
}
