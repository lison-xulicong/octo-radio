#include "radio_frequency_remote.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "nvs.h"

#include <cstdio>
#include <cstdlib>

namespace {

constexpr char kTag[] = "radio_frequency";
constexpr char kCacheNamespace[] = "fmcache1";
constexpr std::size_t kMetadataCapacity = 4096;
constexpr std::size_t kNoFrequencyMemory = 24;

uint32_t s_no_frequency_ids[kNoFrequencyMemory] = {};
std::size_t s_no_frequency_count = 0;

bool recently_missing(uint32_t channel_id) {
    for (std::size_t i = 0; i < s_no_frequency_count; ++i) {
        if (s_no_frequency_ids[i] == channel_id) return true;
    }
    return false;
}

void remember_missing(uint32_t channel_id) {
    if (recently_missing(channel_id)) return;
    if (s_no_frequency_count < kNoFrequencyMemory) {
        s_no_frequency_ids[s_no_frequency_count++] = channel_id;
        return;
    }
    for (std::size_t i = 1; i < kNoFrequencyMemory; ++i) {
        s_no_frequency_ids[i - 1] = s_no_frequency_ids[i];
    }
    s_no_frequency_ids[kNoFrequencyMemory - 1] = channel_id;
}

enum class FetchResult {
    Unavailable,
    Response,
    NotFound,
};

FetchResult fetch_metadata(uint32_t channel_id, char *response,
                           std::size_t capacity) {
    if (!response || capacity < 2) return FetchResult::Unavailable;
    char url[80] = {};
    std::snprintf(url, sizeof(url), "http://rapi.qtfm.cn/channels/%u",
                  static_cast<unsigned>(channel_id));

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 2500;
    config.buffer_size = 1024;
    config.buffer_size_tx = 256;
    config.user_agent = "Octo-Radio/1.0";
    config.keep_alive_enable = false;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return FetchResult::Unavailable;
    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");

    FetchResult result = FetchResult::Unavailable;
    std::size_t used = 0;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status == 404 || status == 410) {
            result = FetchResult::NotFound;
            break;
        }
        if (status < 200 || status >= 300) break;
        result = FetchResult::Response;
        while (used + 1 < capacity) {
            const int received = esp_http_client_read(
                client, response + used, static_cast<int>(capacity - used - 1));
            if (received < 0) {
                used = 0;
                result = FetchResult::Unavailable;
                break;
            }
            if (received == 0) break;
            used += static_cast<std::size_t>(received);
        }
        // A body larger than our small metadata window is malformed for this
        // use, but the service itself is healthy. Let the parser reject it and
        // continue with the next station.
    } while (false);

    response[used] = '\0';
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

void cache_key(uint32_t channel_id, char *target, std::size_t capacity) {
    std::snprintf(target, capacity, "q%08x",
                  static_cast<unsigned>(channel_id));
}

}  // namespace

void radio_frequency_enrich_online(RadioStation *stations, std::size_t count,
                                   RadioFrequencyParser parser) {
    if (!stations || !count || !parser) return;

    nvs_handle_t nvs = 0;
    bool nvs_dirty = false;
    const bool have_nvs =
        nvs_open(kCacheNamespace, NVS_READWRITE, &nvs) == ESP_OK;
    char *response = static_cast<char *>(std::malloc(kMetadataCapacity));
    bool service_available = response != nullptr;

    for (std::size_t i = 0; i < count && service_available; ++i) {
        RadioStation &station = stations[i];
        if (station.frequency_decihz) continue;
        uint32_t channel_id = 0;
        if (!radio_frequency_qingting_channel_id(station.url, &channel_id)) {
            continue;
        }
        if (recently_missing(channel_id)) continue;
        char key_text[16] = {};
        cache_key(channel_id, key_text, sizeof(key_text));

        uint16_t cached = 0;
        if (have_nvs && nvs_get_u16(nvs, key_text, &cached) == ESP_OK &&
            cached >= 870 && cached <= 1080) {
            station.frequency_decihz = cached;
            continue;
        }

        const FetchResult fetched =
            fetch_metadata(channel_id, response, kMetadataCapacity);
        if (fetched == FetchResult::Unavailable) {
            ESP_LOGW(kTag, "Metadata service unavailable at '%s'", station.name);
            service_available = false;
            break;
        }
        if (fetched == FetchResult::NotFound) {
            remember_missing(channel_id);
            continue;
        }

        const uint16_t frequency =
            radio_frequency_parse_metadata(response, parser);
        if (frequency < 870 || frequency > 1080) {
            remember_missing(channel_id);
            continue;
        }
        station.frequency_decihz = frequency;
        if (have_nvs && nvs_set_u16(nvs, key_text, frequency) == ESP_OK) {
            nvs_dirty = true;
        }
        ESP_LOGI(kTag, "Online FM %u.%u for '%s'", frequency / 10U,
                 frequency % 10U, station.name);
    }

    if (have_nvs) {
        if (nvs_dirty) nvs_commit(nvs);
        nvs_close(nvs);
    }
    std::free(response);
}
