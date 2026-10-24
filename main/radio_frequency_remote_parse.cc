#include "radio_frequency_remote_parse.h"

#include "cJSON.h"

#include <cstring>
#include <limits>

namespace {

const char *json_string(cJSON *object, const char *name) {
    cJSON *item = object ? cJSON_GetObjectItemCaseSensitive(object, name) : nullptr;
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

}  // namespace

bool radio_frequency_qingting_channel_id(const char *url,
                                         uint32_t *channel_id) {
    if (!url || !channel_id) return false;
    const bool qingting =
        std::strstr(url, "qtfm.cn/") || std::strstr(url, "qingting.fm/");
    const char *live = qingting ? std::strstr(url, "/live/") : nullptr;
    if (!live) return false;
    const char *p = live + 6;
    if (*p < '0' || *p > '9') return false;
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9') {
        const uint32_t digit = static_cast<uint32_t>(*p - '0');
        if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10U) {
            return false;
        }
        value = value * 10 + digit;
        ++p;
    }
    *channel_id = value;
    return value != 0;
}

uint16_t radio_frequency_parse_metadata(const char *response,
                                        RadioFrequencyParser parser) {
    if (!response || !parser) return 0;
    cJSON *root = cJSON_Parse(response);
    if (!root) return 0;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(root, "Data");
    uint16_t frequency = parser(json_string(data, "description"));
    if (!frequency) frequency = parser(json_string(data, "title"));
    cJSON_Delete(root);
    return frequency;
}
