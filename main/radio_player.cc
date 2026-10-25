#include "radio_player.h"

#include "bsp_audio.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "nvs.h"
#include "radio_mp3_sync.h"
#include "radio_ui.h"

#include "decoder/esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr char kTag[] = "radio_player";
// Larger than one maximum-size MPEG-1 Layer III frame (1441 bytes). This lets
// frame sync confirm the next header even for 256/320 kbps streams.
constexpr std::size_t kInputSize = 1536;
constexpr std::size_t kJitterCapacities[] = {8 * 1024, 6 * 1024, 4 * 1024};
constexpr std::size_t kJitterReaderStack = 4096;
constexpr UBaseType_t kPlaybackTaskPriority = 6;
constexpr UBaseType_t kJitterTaskPriority = 5;
// Stream reads use the HTTP client's 3 s receive timeout. It should therefore
// stop within roughly 3.1 s. Log a warning if it does not, but never delete a
// task blocked in lwIP: that leaves a stale socket waiter behind and can assert
// when the HTTP client is closed.
constexpr uint32_t kJitterStopWarningMs = 5000;
// One MP3 frame is at most 1152 stereo samples (4608 bytes). 5 KiB leaves room
// for decoder-added frame data without consuming scarce internal RAM.
constexpr std::size_t kOutputSize = 5 * 1024;
// Held across HTTP setup, then released exactly before decoding. The MP3
// decoder's largest allocation is 8 KiB; reserving more than that failed on
// station changes once the larger jitter buffer had fragmented the heap.
constexpr std::size_t kDecoderReserve = 8 * 1024;
constexpr std::size_t kLevelCount = 18;
constexpr std::size_t kMaxStations = 8;
constexpr uint32_t kLevelUpdatePeriodMs = 40;
constexpr uint32_t kVolumeSaveDelayMs = 800;
// Consecutive silent failures tolerated before moving to the next station.
constexpr uint8_t kMaxFailedAttempts = 3;

const RadioStation kFallbackStations[] = {
    {"中国之声", "新闻综合 · 网络直播",
     "http://lhttp.qingting.fm/live/15318317/64k.mp3", 0},
    {"怀旧音乐", "经典老歌 · 网络直播",
     "http://lhttp.qingting.fm/live/4804/64k.mp3", 0},
    {"清晨音乐", "轻松旋律 · 网络直播",
     "http://lhttp.qingting.fm/live/4915/64k.mp3", 0},
    {"两广之声", "粤语音乐 · 网络直播",
     "http://lhttp.qingting.fm/live/20500149/64k.mp3", 0},
    {"羊城交通", "城市资讯 · 网络直播",
     "http://lhttp.qingting.fm/live/1262/64k.mp3", 0},
    {"第一财经", "财经资讯 · 网络直播",
     "http://lhttp.qingting.fm/live/276/64k.mp3", 0},
};

std::atomic<bool> s_network_connected{false};
std::atomic<bool> s_wanted_playing{true};
std::atomic<std::size_t> s_station_index{0};
std::atomic<std::size_t> s_station_count{0};
std::atomic<uint8_t> s_volume{55};
std::atomic<bool> s_volume_save_pending{false};
std::atomic<uint32_t> s_volume_save_due_ms{0};
std::atomic<uint32_t> s_generation{1};
std::atomic<bool> s_stream_active{false};
TaskHandle_t s_player_task;
SemaphoreHandle_t s_station_mutex;
RadioStation s_stations[kMaxStations] = {};
void *s_decoder_reserve;
uint8_t s_level_curve[257];
uint32_t s_last_level_update_ms;

bool ensure_decoder_reserve() {
    if (s_decoder_reserve) return true;
    s_decoder_reserve =
        heap_caps_malloc(kDecoderReserve, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(kTag, "Decoder reserve %s (%u bytes), free=%u largest=%u",
             s_decoder_reserve ? "ready" : "FAILED",
             static_cast<unsigned>(kDecoderReserve),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    return s_decoder_reserve != nullptr;
}

void release_decoder_reserve() {
    heap_caps_free(s_decoder_reserve);
    s_decoder_reserve = nullptr;
}

RadioStation station_snapshot(std::size_t index) {
    RadioStation station = {};
    if (!s_station_mutex ||
        xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(250)) != pdTRUE) {
        return station;
    }
    const std::size_t count = std::max<std::size_t>(s_station_count.load(), 1);
    station = s_stations[index % count];
    xSemaphoreGive(s_station_mutex);
    return station;
}

void save_setting(const char *key, uint8_t value) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, key, value);
    nvs_commit(handle);
    nvs_close(handle);
}

uint8_t load_setting(const char *key, uint8_t fallback) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return fallback;
    uint8_t value = fallback;
    if (nvs_get_u8(handle, key, &value) != ESP_OK) value = fallback;
    nvs_close(handle);
    return value;
}

void save_station_choice(std::size_t index, const char *name) {
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READWRITE, &handle) != ESP_OK) return;
    nvs_set_u8(handle, "station", static_cast<uint8_t>(index));
    if (name && name[0]) {
        nvs_set_str(handle, "station_name", name);
    } else {
        nvs_erase_key(handle, "station_name");
    }
    nvs_commit(handle);
    nvs_close(handle);
}

void persist_volume_if_due(bool force = false) {
    if (!s_volume_save_pending.load(std::memory_order_acquire)) return;
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (!force && static_cast<int32_t>(now_ms - s_volume_save_due_ms.load(
                                             std::memory_order_acquire)) < 0) {
        return;
    }
    s_volume_save_pending.store(false, std::memory_order_release);
    const uint8_t saved = s_volume.load(std::memory_order_acquire);
    save_setting("volume", saved);
    if (s_volume.load(std::memory_order_acquire) != saved) {
        s_volume_save_due_ms.store(now_ms + kVolumeSaveDelayMs,
                                   std::memory_order_release);
        s_volume_save_pending.store(true, std::memory_order_release);
    }
}

// Copies the station name the listener last chose. Returns false when no
// station has been remembered yet.
bool load_station_name(char *name, std::size_t capacity) {
    if (!name || capacity == 0) return false;
    name[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open("radio", NVS_READONLY, &handle) != ESP_OK) return false;
    std::size_t length = capacity;
    const esp_err_t error = nvs_get_str(handle, "station_name", name, &length);
    nvs_close(handle);
    if (error != ESP_OK) {
        name[0] = '\0';
        return false;
    }
    return name[0] != '\0';
}

// Installs a catalog under the station mutex and reports the index that should
// stay selected. `preferred_name` keeps the previous station when the fresh
// catalog still carries it.
bool install_stations(const RadioStation *stations, std::size_t count,
                      const char *preferred_name, std::size_t *selected) {
    if (!stations || count == 0 || !s_station_mutex) return false;
    count = std::min(count, kMaxStations);

    // Stop the current request before replacing its backing catalog. The
    // streaming task uses a value snapshot, so the swap is atomic to users.
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    if (xSemaphoreTake(s_station_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
    std::memcpy(s_stations, stations, count * sizeof(RadioStation));
    if (count < kMaxStations) {
        std::memset(s_stations + count, 0,
                    (kMaxStations - count) * sizeof(RadioStation));
    }
    std::size_t index = 0;
    if (preferred_name && preferred_name[0]) {
        for (std::size_t i = 0; i < count; ++i) {
            if (std::strcmp(s_stations[i].name, preferred_name) == 0) {
                index = i;
                break;
            }
        }
    }
    s_station_count.store(count, std::memory_order_release);
    s_station_index.store(index, std::memory_order_release);
    xSemaphoreGive(s_station_mutex);
    if (selected) *selected = index;
    return true;
}

bool request_still_current(uint32_t generation, std::size_t station) {
    return s_network_connected.load(std::memory_order_acquire) &&
           s_wanted_playing.load(std::memory_order_acquire) &&
           s_generation.load(std::memory_order_acquire) == generation &&
           s_station_index.load(std::memory_order_acquire) == station;
}

struct JitterReader {
    StreamBufferHandle_t buffer;
    esp_http_client_handle_t client;
    uint32_t generation;
    std::size_t station;
    std::atomic<bool> stop;
    std::atomic<bool> failed;
    std::atomic<bool> done;
};

enum class StreamResult {
    Played,
    Retry,
    Unavailable,
};

void jitter_reader_task(void *argument) {
    auto *reader = static_cast<JitterReader *>(argument);
    uint8_t chunk[kInputSize];
    int empty_reads = 0;

    while (!reader->stop.load(std::memory_order_acquire) &&
           request_still_current(reader->generation, reader->station)) {
        const int received = esp_http_client_read(
            reader->client, reinterpret_cast<char *>(chunk), sizeof(chunk));
        if (received < 0) {
            ESP_LOGW(kTag, "Stream read error");
            reader->failed.store(true, std::memory_order_release);
            break;
        }
        if (received == 0) {
            if (++empty_reads > 3) {
                reader->failed.store(true, std::memory_order_release);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        empty_reads = 0;

        std::size_t used = 0;
        while (used < static_cast<std::size_t>(received) &&
               !reader->stop.load(std::memory_order_acquire)) {
            const std::size_t sent = xStreamBufferSend(
                reader->buffer, chunk + used,
                static_cast<std::size_t>(received) - used,
                pdMS_TO_TICKS(100));
            if (!sent) {
                if (reader->stop.load(std::memory_order_acquire)) break;
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            used += sent;
        }
    }

    reader->done.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}

bool start_jitter_reader(esp_http_client_handle_t client, uint32_t generation,
                         std::size_t station, JitterReader *jitter,
                         std::size_t *capacity, TaskHandle_t *task) {
    if (!jitter || !capacity || !task) return false;
    *capacity = 0;
    *task = nullptr;
    for (std::size_t candidate : kJitterCapacities) {
        jitter->buffer = xStreamBufferCreate(candidate, 1);
        if (!jitter->buffer) continue;
        jitter->client = client;
        jitter->generation = generation;
        jitter->station = station;
        jitter->stop.store(false, std::memory_order_release);
        jitter->failed.store(false, std::memory_order_release);
        jitter->done.store(false, std::memory_order_release);
        if (xTaskCreate(jitter_reader_task, "radio_jitter", kJitterReaderStack,
                        jitter, kJitterTaskPriority, task) == pdPASS) {
            *capacity = candidate;
            return true;
        }
        vStreamBufferDelete(jitter->buffer);
        jitter->buffer = nullptr;
    }
    return false;
}

uint32_t integer_sqrt(uint32_t value) {
    uint32_t result = 0;
    uint32_t bit = 1u << 30;
    while (bit > value) bit >>= 2;
    while (bit) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }
    return result;
}

void init_level_curve() {
    for (std::size_t i = 0; i < sizeof(s_level_curve); ++i) {
        const int rms = static_cast<int>(i) * 128;
        const float db = rms > 1 ? 20.0f * std::log10(rms / 32768.0f) : -80.0f;
        const int value = static_cast<int>((db + 58.0f) * 2.25f);
        s_level_curve[i] = static_cast<uint8_t>(std::clamp(value, 2, 100));
    }
}

void calculate_levels(const uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (!pcm || bytes < 64) return;
    const uint32_t now_ms = static_cast<uint32_t>(esp_timer_get_time() / 1000);
    if (static_cast<uint32_t>(now_ms - s_last_level_update_ms) <
        kLevelUpdatePeriodMs) {
        return;
    }
    s_last_level_update_ms = now_ms;
    const auto *samples = reinterpret_cast<const int16_t *>(pcm);
    const std::size_t sample_count = bytes / sizeof(int16_t);
    if (sample_count < kLevelCount) return;
    const std::size_t frames = sample_count / std::max<uint8_t>(channels, 1);
    if (frames < kLevelCount) return;

    uint8_t levels[kLevelCount] = {};
    const uint32_t generation = s_generation.load(std::memory_order_acquire);
    for (std::size_t band = 0; band < kLevelCount; ++band) {
        const std::size_t begin = band * frames / kLevelCount;
        const std::size_t end = (band + 1) * frames / kLevelCount;
        int64_t energy = 0;
        std::size_t count = 0;
        for (std::size_t frame = begin; frame < end; ++frame) {
            int32_t mixed = 0;
            for (uint8_t channel = 0; channel < channels; ++channel) {
                mixed += samples[frame * channels + channel];
            }
            mixed /= std::max<uint8_t>(channels, 1);
            energy += static_cast<int64_t>(mixed) * mixed;
            ++count;
        }
        const uint32_t rms =
            count ? integer_sqrt(static_cast<uint32_t>(energy / count)) : 0;
        const std::size_t curve_index = std::min<std::size_t>(
            256, (static_cast<std::size_t>(rms) + 64) >> 7);
        int value = s_level_curve[curve_index];
        value += static_cast<int>((band * 13 + generation) % 9) - 4;
        levels[band] = static_cast<uint8_t>(std::clamp(value, 2, 100));
    }
    radio_ui_set_audio_levels(levels, kLevelCount);
}

std::size_t downmix_to_mono(uint8_t *pcm, std::size_t bytes, uint8_t channels) {
    if (!pcm || channels == 0) return 0;
    if (channels == 1) return bytes & ~static_cast<std::size_t>(1);

    auto *samples = reinterpret_cast<int16_t *>(pcm);
    const std::size_t frames = bytes / (sizeof(int16_t) * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        int32_t mixed = 0;
        for (uint8_t channel = 0; channel < channels; ++channel) {
            mixed += samples[frame * channels + channel];
        }
        samples[frame] = static_cast<int16_t>(mixed / channels);
    }
    return frames * sizeof(int16_t);
}

esp_audio_simple_dec_handle_t open_mp3_decoder() {
    esp_audio_simple_dec_cfg_t config = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t decoder = nullptr;
    const esp_audio_err_t error = esp_audio_simple_dec_open(&config, &decoder);
    if (error != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(kTag, "MP3 decoder open failed: %d", error);
        return nullptr;
    }
    return decoder;
}

StreamResult stream_station(std::size_t station, uint32_t generation) {
    if (!request_still_current(generation, station)) return StreamResult::Retry;
    const RadioStation preset = station_snapshot(station);
    if (!preset.url[0]) {
        ESP_LOGE(kTag, "Station %u has no stream URL", static_cast<unsigned>(station));
        return StreamResult::Unavailable;
    }
    radio_ui_set_playback(RadioPlaybackState::Connecting, "正在连接电台");

    if (!ensure_decoder_reserve()) {
        radio_ui_set_playback(RadioPlaybackState::Error, "内存不足");
        return StreamResult::Retry;
    }

    esp_http_client_config_t config = {};
    config.url = preset.url;
    config.timeout_ms = 3000;
    config.buffer_size = 1024;
    config.buffer_size_tx = 512;
    config.user_agent = "Octo-Radio/1.1";
    config.keep_alive_enable = true;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 4;
    if (std::strncmp(preset.url, "https://", 8) == 0) {
        config.crt_bundle_attach = esp_crt_bundle_attach;
    }
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(kTag, "HTTP client allocation failed");
        return StreamResult::Retry;
    }
    esp_http_client_set_header(client, "Icy-MetaData", "0");
    esp_http_client_set_header(client, "Accept", "audio/mpeg,*/*");

    bool played_audio = false;
    bool unavailable = false;
    esp_audio_simple_dec_handle_t decoder = nullptr;
    uint8_t decoder_input[kInputSize];
    uint8_t *output = nullptr;
    JitterReader jitter = {};
    std::size_t jitter_capacity = 0;
    TaskHandle_t jitter_task = nullptr;
    bool jitter_started = false;

    do {
        if (esp_http_client_open(client, 0) != ESP_OK) {
            ESP_LOGW(kTag, "Open failed: %s", preset.url);
            break;
        }
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(kTag, "HTTP status %d", status);
            unavailable = status == 404 || status == 410;
            break;
        }
        if (!request_still_current(generation, station)) break;

        decoder = open_mp3_decoder();
        if (!decoder) break;
        if (!request_still_current(generation, station)) break;
        output = static_cast<uint8_t *>(malloc(kOutputSize));
        if (!output) {
            ESP_LOGE(kTag, "Not enough memory for stream buffers");
            break;
        }
        release_decoder_reserve();
        ESP_LOGI(kTag, "Decoder start heap free=%u largest=%u (%u B output)",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(kOutputSize));

        radio_ui_set_playback(RadioPlaybackState::Buffering, "正在缓冲");
        bool format_ready = false;
        bool decoder_started = false;
        bool playback_ready = false;
        bool announce_playing = false;
        uint8_t source_channels = 1;
        int empty_reads = 0;
        bool fatal_decode_error = false;
        std::size_t unsynced_bytes = 0;
        std::size_t pending_input = 0;
        uint32_t underruns = 0;

        auto wait_for_jitter = [&]() {
            const std::size_t prefill = jitter_capacity * 7 / 8;
            while (request_still_current(generation, station) &&
                   xStreamBufferBytesAvailable(jitter.buffer) < prefill &&
                   !jitter.failed.load(std::memory_order_acquire) &&
                   !jitter.done.load(std::memory_order_acquire)) {
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (!request_still_current(generation, station)) return false;
            const std::size_t primed =
                xStreamBufferBytesAvailable(jitter.buffer);
            if (!primed) return false;
            playback_ready = true;
            announce_playing = true;
            ESP_LOGI(kTag, "Jitter buffer primed with %u/%u bytes",
                     static_cast<unsigned>(primed),
                     static_cast<unsigned>(prefill));
            return true;
        };

        while (request_still_current(generation, station)) {
            if (pending_input >= sizeof(decoder_input)) {
                ESP_LOGE(kTag, "Decoder input stalled with %u buffered bytes",
                         static_cast<unsigned>(pending_input));
                break;
            }
            int received = 0;
            if (jitter.buffer) {
                received = static_cast<int>(xStreamBufferReceive(
                    jitter.buffer, decoder_input + pending_input,
                    sizeof(decoder_input) - pending_input,
                    pdMS_TO_TICKS(50)));
                if (received == 0) {
                    const bool ended =
                        jitter.failed.load(std::memory_order_acquire) ||
                        jitter.done.load(std::memory_order_acquire);
                    if (ended && !xStreamBufferBytesAvailable(jitter.buffer)) break;
                    if (format_ready) {
                        if (playback_ready) {
                            playback_ready = false;
                            ++underruns;
                            ESP_LOGW(kTag, "Audio buffer underrun (%u)",
                                     static_cast<unsigned>(underruns));
                            radio_ui_set_playback(RadioPlaybackState::Buffering,
                                                  "正在缓冲");
                        }
                        if (!wait_for_jitter()) {
                            if (ended) break;
                        }
                    }
                    continue;
                }
            } else {
                received = esp_http_client_read(client,
                                                reinterpret_cast<char *>(
                                                    decoder_input + pending_input),
                                                kInputSize - pending_input);
                if (received < 0) {
                    ESP_LOGW(kTag, "Stream read error");
                    break;
                }
                if (received == 0) {
                    if (++empty_reads > 3) break;
                    vTaskDelay(pdMS_TO_TICKS(20));
                    continue;
                }
                empty_reads = 0;
            }

            const std::size_t available =
                pending_input + static_cast<std::size_t>(received);
            std::size_t frame_offset = 0;
            if (!decoder_started) {
                frame_offset = octo_audio::find_mp3_frame(decoder_input, available);
                if (frame_offset == available) {
                    const std::size_t keep = std::min<std::size_t>(
                        available, octo_audio::kMp3MaxFrameSize + 4);
                    unsynced_bytes += available - keep;
                    if (unsynced_bytes > 64 * 1024) {
                        ESP_LOGE(kTag, "No MP3 frame found in %u bytes",
                                 static_cast<unsigned>(unsynced_bytes));
                        break;
                    }
                    if (keep < available) {
                        std::memmove(decoder_input, decoder_input + available - keep,
                                     keep);
                    }
                    pending_input = keep;
                    continue;
                }
                if (frame_offset) {
                    ESP_LOGI(kTag, "Skipped %u bytes before MP3 frame",
                             static_cast<unsigned>(frame_offset));
                }
                decoder_started = true;
            }

            esp_audio_simple_dec_raw_t raw = {
                .buffer = decoder_input + frame_offset,
                .len = static_cast<uint32_t>(available - frame_offset),
                .eos = false,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            while (raw.len > 0 && request_still_current(generation, station)) {
                esp_audio_simple_dec_out_t frame = {
                    .buffer = output,
                    .len = kOutputSize,
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                const uint32_t before = raw.len;
                const esp_audio_err_t result =
                    esp_audio_simple_dec_process(decoder, &raw, &frame);
                if (result == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    ESP_LOGE(kTag, "Decoder needs %u bytes, buffer has %u",
                             static_cast<unsigned>(frame.needed_size),
                             static_cast<unsigned>(kOutputSize));
                    fatal_decode_error = true;
                    raw.len = 0;
                    break;
                }
                if (result != ESP_AUDIO_ERR_OK) {
                    ESP_LOGW(kTag, "MP3 decode failed: %d (heap free=%u largest=%u)",
                             result,
                             static_cast<unsigned>(
                                 heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                             static_cast<unsigned>(
                                 heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
                    fatal_decode_error = true;
                    raw.len = 0;
                    break;
                }
                if (raw.consumed > raw.len) raw.consumed = raw.len;
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;

                if (frame.decoded_size > 0) {
                    esp_audio_simple_dec_info_t info = {};
                    if (!format_ready &&
                        esp_audio_simple_dec_get_info(decoder, &info) == ESP_AUDIO_ERR_OK) {
                        source_channels = std::clamp<uint8_t>(info.channel, 1, 2);
                        if (info.bits_per_sample != 16 || info.sample_rate < 8000 ||
                            info.sample_rate > 48000 ||
                            bsp_audio_set_format(info.sample_rate, 16, 1) != ESP_OK) {
                            ESP_LOGE(kTag, "Unsupported stream format: %luHz/%ubit/%uch",
                                     static_cast<unsigned long>(info.sample_rate),
                                     info.bits_per_sample, source_channels);
                            fatal_decode_error = true;
                            raw.len = 0;
                            break;
                        }
                        bsp_audio_set_volume(s_volume.load(std::memory_order_acquire));
                        format_ready = true;
                        ESP_LOGI(kTag, "MP3 stream %s at %luHz/%uch -> mono", preset.name,
                                 static_cast<unsigned long>(info.sample_rate), source_channels);
                    }
                    if (format_ready && !jitter_started) {
                        jitter_started = true;
                        start_jitter_reader(client, generation, station, &jitter,
                                            &jitter_capacity, &jitter_task);
                        if (jitter.buffer) {
                            ESP_LOGI(kTag,
                                     "Jitter ready (%u B), heap free=%u largest=%u",
                                     static_cast<unsigned>(jitter_capacity),
                                     static_cast<unsigned>(
                                         heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                                     static_cast<unsigned>(
                                         heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
                        } else {
                            playback_ready = true;
                            announce_playing = true;
                            ESP_LOGW(kTag, "Jitter unavailable; using direct stream");
                        }
                    }
                    if (format_ready && playback_ready) {
                        const std::size_t mono_bytes =
                            downmix_to_mono(frame.buffer, frame.decoded_size, source_channels);
                        if (bsp_audio_write(frame.buffer, mono_bytes) != ESP_OK) continue;
                        calculate_levels(frame.buffer, mono_bytes, 1);
                        if (announce_playing) {
                            announce_playing = false;
                            played_audio = true;
                            radio_ui_set_playback(RadioPlaybackState::Playing, "正在播放");
                        }
                    }
                }

                if (raw.len == before || raw.consumed == 0) {
                    // The parser may cache a short incomplete tail. A fresh network
                    // block is required; keeping this block would spin forever.
                    break;
                }
            }
            pending_input = raw.len;
            if (pending_input && raw.buffer != decoder_input) {
                std::memmove(decoder_input, raw.buffer, pending_input);
            }
            if (format_ready && jitter.buffer && !playback_ready) {
                if (!wait_for_jitter()) break;
            }
            if (fatal_decode_error) break;
        }
    } while (false);

    if (jitter.buffer) {
        jitter.stop.store(true, std::memory_order_release);
        if (jitter_task) {
            const TickType_t deadline =
                xTaskGetTickCount() + pdMS_TO_TICKS(kJitterStopWarningMs);
            bool warned = false;
            while (!jitter.done.load(std::memory_order_acquire)) {
                if (!warned &&
                    static_cast<int32_t>(deadline - xTaskGetTickCount()) <= 0) {
                    ESP_LOGE(kTag, "Jitter reader is still stopping");
                    warned = true;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }
        vStreamBufferDelete(jitter.buffer);
    }
    if (decoder) esp_audio_simple_dec_close(decoder);
    free(output);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (unavailable) return StreamResult::Unavailable;
    return played_audio ? StreamResult::Played : StreamResult::Retry;
}

void player_task(void *) {
    std::size_t last_attempted_station = SIZE_MAX;
    uint8_t failed_attempts = 0;
    while (true) {
        uint32_t request_generation = 0;
        std::size_t station = 0;
        bool connected = false;
        bool wanted = false;
        do {
            request_generation = s_generation.load(std::memory_order_acquire);
            connected = s_network_connected.load(std::memory_order_acquire);
            wanted = s_wanted_playing.load(std::memory_order_acquire);
            station = s_station_index.load(std::memory_order_acquire);
        } while (s_generation.load(std::memory_order_acquire) !=
                 request_generation);

        if (!connected) {
            radio_ui_set_playback(RadioPlaybackState::Stopped, "等待网络");
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }
        if (!wanted) {
            radio_ui_set_playback(RadioPlaybackState::Stopped, "已暂停");
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }

        const uint32_t generation = request_generation;
        if (station != last_attempted_station) {
            last_attempted_station = station;
            failed_attempts = 0;
        }
        s_stream_active.store(true, std::memory_order_release);
        const StreamResult result = stream_station(station, generation);
        s_stream_active.store(false, std::memory_order_release);
        if (!request_still_current(generation, station)) continue;

        if (result == StreamResult::Unavailable) {
            const std::size_t count = radio_player_station_count();
            if (count > 1) {
                radio_ui_set_playback(RadioPlaybackState::Error,
                                      "电台已停播 正在换台");
                vTaskDelay(pdMS_TO_TICKS(600));
                radio_player_select_relative(1);
                continue;
            }
            radio_ui_set_playback(RadioPlaybackState::Error, "电台已停播");
            vTaskDelay(pdMS_TO_TICKS(10000));
            continue;
        }

        if (result == StreamResult::Played) {
            // The stream worked at least briefly, so this station is alive and
            // simply dropped. Keep it and reconnect.
            failed_attempts = 0;
            radio_ui_set_playback(RadioPlaybackState::Reconnecting,
                                  "信号中断 正在重连");
            vTaskDelay(pdMS_TO_TICKS(1200));
            continue;
        }

        ++failed_attempts;
        const std::size_t count = radio_player_station_count();
        // Public directories keep dead entries around. After a few silent
        // failures, move on instead of retrying the same address forever.
        if (failed_attempts >= kMaxFailedAttempts && count > 1) {
            ESP_LOGW(kTag, "Station %u unreachable, skipping to the next one",
                     static_cast<unsigned>(station));
            radio_ui_set_playback(RadioPlaybackState::Error,
                                  "电台无法播放 已换台");
            vTaskDelay(pdMS_TO_TICKS(1200));
            failed_attempts = 0;
            radio_player_select_relative(1);
            continue;
        }

        radio_ui_set_playback(RadioPlaybackState::Error, "连接失败 正在重试");
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

}  // namespace

bool radio_player_init() {
    s_station_mutex = xSemaphoreCreateMutex();
    if (!s_station_mutex) return false;
    std::memcpy(s_stations, kFallbackStations, sizeof(kFallbackStations));
    s_station_count.store(sizeof(kFallbackStations) / sizeof(kFallbackStations[0]));

    const uint8_t saved_station = load_setting("station", 0);
    const uint8_t saved_volume = load_setting("volume", 55);
    s_station_index.store(saved_station % radio_player_station_count());
    s_volume.store(std::clamp<uint8_t>(saved_volume, 10, 100));
    init_level_curve();

    if (bsp_audio_init() != ESP_OK) return false;
    if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    if (esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) return false;
    bsp_audio_set_volume(s_volume.load());
    ensure_decoder_reserve();

    RadioStation startup_station =
        radio_player_station(s_station_index.load());
    char remembered_name[sizeof(startup_station.name)] = {};
    if (load_station_name(remembered_name, sizeof(remembered_name))) {
        std::snprintf(startup_station.name, sizeof(startup_station.name), "%s",
                      remembered_name);
        std::snprintf(startup_station.description,
                      sizeof(startup_station.description),
                      "已保存频道 · 等待网络");
    }
    radio_ui_set_station(s_station_index.load(), radio_player_station_count(),
                         startup_station);
    radio_ui_set_volume(s_volume.load());
    if (xTaskCreate(player_task, "radio_stream", 8192, nullptr,
                    kPlaybackTaskPriority, &s_player_task) != pdPASS) {
        s_player_task = nullptr;
        return false;
    }
    return true;
}

void radio_player_persist_pending_volume() { persist_volume_if_due(); }

void radio_player_set_network(bool connected) {
    // Publish the state first and the generation last. The playback task uses
    // this as a seqlock, so it can never pair a new generation with the old
    // connection/selection state.
    s_network_connected.store(connected, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
}

void radio_player_release_decoder_memory() { release_decoder_reserve(); }

void radio_player_toggle() {
    radio_player_set_playing(!radio_player_is_playing());
}

void radio_player_set_playing(bool playing) {
    s_wanted_playing.store(playing, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    if (!playing) {
        persist_volume_if_due(true);
        radio_ui_set_playback(RadioPlaybackState::Stopped, "已暂停");
    }
}

bool radio_player_is_playing() {
    return s_wanted_playing.load(std::memory_order_acquire);
}

bool radio_player_wait_idle(uint32_t timeout_ms) {
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    while (s_stream_active.load(std::memory_order_acquire)) {
        if (static_cast<int32_t>(deadline - xTaskGetTickCount()) <= 0) return false;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

void radio_player_select_relative(int delta) {
    const int count = static_cast<int>(radio_player_station_count());
    if (count <= 1) {
        ESP_LOGW(kTag, "Cannot change station: catalog has %d entry", count);
        return;
    }
    const int current = static_cast<int>(s_station_index.load());
    const std::size_t next = static_cast<std::size_t>((current + delta + count) % count);
    radio_player_set_station(next);
}

void radio_player_set_station(std::size_t index) {
    index %= radio_player_station_count();
    const RadioStation chosen = radio_player_station(index);

    // Update the visible selection before touching NVS. Saving can block long
    // enough to make a key press feel ignored, and the stream task only needs
    // the new generation/index to abandon the old connection.
    s_station_index.store(index, std::memory_order_release);
    s_generation.fetch_add(1, std::memory_order_acq_rel);
    const std::size_t count = radio_player_station_count();
    radio_ui_set_station(index, count, chosen);
    if (radio_player_is_playing()) {
        radio_ui_set_playback(RadioPlaybackState::Connecting, "正在切换电台");
    }
    persist_volume_if_due(true);
    save_station_choice(index, chosen.name);
    ESP_LOGI(kTag, "Selected station %u/%u: %s",
             static_cast<unsigned>(index + 1), static_cast<unsigned>(count), chosen.name);
}

std::size_t radio_player_station_index() {
    return s_station_index.load(std::memory_order_acquire);
}

std::size_t radio_player_station_count() {
    return std::max<std::size_t>(s_station_count.load(std::memory_order_acquire), 1);
}

RadioStation radio_player_station(std::size_t index) {
    return station_snapshot(index);
}

bool radio_player_replace_stations(const RadioStation *stations, std::size_t count) {
    std::size_t selected = 0;
    if (!install_stations(stations, count, nullptr, &selected)) return false;

    const RadioStation chosen = radio_player_station(selected);
    persist_volume_if_due(true);
    save_station_choice(selected, chosen.name);
    radio_ui_set_station(selected, radio_player_station_count(), chosen);
    ESP_LOGI(kTag, "Installed local catalog with %u stations",
             static_cast<unsigned>(radio_player_station_count()));
    return true;
}

bool radio_player_replace_stations_resuming(const RadioStation *stations,
                                           std::size_t count) {
    char remembered[sizeof(RadioStation::name)] = {};
    const bool has_remembered = load_station_name(remembered, sizeof(remembered));

    std::size_t selected = 0;
    if (!install_stations(stations, count, has_remembered ? remembered : nullptr,
                          &selected)) {
        return false;
    }

    const RadioStation chosen = radio_player_station(selected);
    persist_volume_if_due(true);
    save_station_choice(selected, chosen.name);
    radio_ui_set_station(selected, radio_player_station_count(), chosen);
    if (has_remembered && selected > 0) {
        ESP_LOGI(kTag, "Resumed remembered station '%s' at index %u",
                 chosen.name, static_cast<unsigned>(selected));
    } else {
        ESP_LOGI(kTag, "Installed local catalog with %u stations",
                 static_cast<unsigned>(radio_player_station_count()));
    }
    return true;
}

void radio_player_set_volume(uint8_t volume) {
    volume = std::clamp<uint8_t>(volume, 10, 100);
    s_volume.store(volume, std::memory_order_release);
    bsp_audio_set_volume(volume);
    radio_ui_set_volume(volume);
    s_volume_save_due_ms.store(
        static_cast<uint32_t>(esp_timer_get_time() / 1000) + kVolumeSaveDelayMs,
        std::memory_order_release);
    s_volume_save_pending.store(true, std::memory_order_release);
}

uint8_t radio_player_volume() {
    return s_volume.load(std::memory_order_acquire);
}
