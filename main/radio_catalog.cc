#include "radio_catalog.h"
#include "radio_cities.h"
#include "radio_frequency.h"
#ifdef CATALOG_TESTING
#include "radio_frequency_remote_stub.h"
#else
#include "radio_frequency_remote.h"
#endif

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

constexpr char kTag[] = "radio_catalog";
constexpr char kLocationUrl[] =
    "http://ip-api.com/json/?fields=status,countryCode,regionName,city";
constexpr char kBackupLocationUrl[] =
    "https://ipwho.is/?fields=success,country_code,region,city";
// The directory has no load balancer: each mirror is its own host, and a dead
// one has to be skipped by the client. These two answer today; the others the
// project lists (at1, nl1, fi1) no longer resolve.
constexpr const char *kDirectoryHosts[] = {
    "http://de1.api.radio-browser.info/json/stations/search",
    "http://de2.api.radio-browser.info/json/stations/search",
};

constexpr char kCountry[] = "CN";
// Shown on the dial, and used to name stations whose own name the display font
// cannot spell. Both glyphs are in the project font.
constexpr char kNationwide[] = "中国";
// Eight filtered stations fit comfortably in 16 KiB. The old 24 KiB buffer
// could not be allocated while the player was closing its previous HTTP
// stream, which made a city switch silently leave the old catalog installed.
constexpr std::size_t kResponseCapacity = 16384;

struct CityTranslation {
    const char *english;
    const char *chinese;
};

// ip-api returns English city names. Radio Browser's Chinese entries are much
// easier to find by their Chinese city name, so map the most common cities and
// fall back to a province/state query for every other place.
constexpr CityTranslation kChineseCities[] = {
    {"Beijing", "北京"},       {"Shanghai", "上海"},
    {"Tianjin", "天津"},       {"Chongqing", "重庆"},
    {"Guangzhou", "广州"},     {"Shenzhen", "深圳"},
    {"Dongguan", "东莞"},      {"Foshan", "佛山"},
    {"Zhuhai", "珠海"},        {"Huizhou", "惠州"},
    {"Zhongshan", "中山"},     {"Shantou", "汕头"},
    {"Hangzhou", "杭州"},      {"Ningbo", "宁波"},
    {"Wenzhou", "温州"},       {"Jiaxing", "嘉兴"},
    {"Shaoxing", "绍兴"},      {"Jinhua", "金华"},
    {"Nanjing", "南京"},       {"Suzhou", "苏州"},
    {"Wuxi", "无锡"},          {"Changzhou", "常州"},
    {"Nantong", "南通"},       {"Xuzhou", "徐州"},
    {"Chengdu", "成都"},       {"Mianyang", "绵阳"},
    {"Wuhan", "武汉"},         {"Yichang", "宜昌"},
    {"Changsha", "长沙"},      {"Zhengzhou", "郑州"},
    {"Luoyang", "洛阳"},       {"Jinan", "济南"},
    {"Qingdao", "青岛"},       {"Yantai", "烟台"},
    {"Shenyang", "沈阳"},      {"Dalian", "大连"},
    {"Harbin", "哈尔滨"},      {"Changchun", "长春"},
    {"Shijiazhuang", "石家庄"}, {"Taiyuan", "太原"},
    {"Xi'an", "西安"},         {"Xian", "西安"},
    {"Kunming", "昆明"},       {"Guiyang", "贵阳"},
    {"Nanning", "南宁"},       {"Haikou", "海口"},
    {"Fuzhou", "福州"},        {"Xiamen", "厦门"},
    {"Nanchang", "南昌"},      {"Hefei", "合肥"},
    {"Lanzhou", "兰州"},       {"Urumqi", "乌鲁木齐"},
    {"Hohhot", "呼和浩特"},    {"Yinchuan", "银川"},
    {"Xining", "西宁"},        {"Lhasa", "拉萨"},
};

// The whole country, for when no city can be pinned down.
RadioLocation nationwide() {
    RadioLocation location = {};
    std::snprintf(location.country_code, sizeof(location.country_code), "%s", kCountry);
    std::snprintf(location.region, sizeof(location.region), "%s", kNationwide);
    std::snprintf(location.label, sizeof(location.label), "%s", kNationwide);
    std::snprintf(location.place, sizeof(location.place), "%s", kNationwide);
    return location;
}

bool starts_with(const char *text, const char *prefix) {
    return text && prefix && std::strncmp(text, prefix, std::strlen(prefix)) == 0;
}

bool contains(const char *text, const char *needle) {
    return text && needle && std::strstr(text, needle) != nullptr;
}

bool http_get(const char *url, char *response, std::size_t capacity,
              std::size_t *response_size) {
    if (!url || !response || capacity < 2) return false;
    response[0] = '\0';

    esp_http_client_config_t config = {};
    config.url = url;
    config.timeout_ms = 5000;
    config.buffer_size = 2048;
    config.buffer_size_tx = 512;
    config.user_agent = "Octo-Radio/1.1";
    config.keep_alive_enable = false;
    config.disable_auto_redirect = false;
    config.max_redirection_count = 3;
    if (starts_with(url, "https://")) config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return false;

    esp_http_client_set_header(client, "Accept", "application/json");
    esp_http_client_set_header(client, "Accept-Encoding", "identity");
    bool success = false;
    std::size_t used = 0;
    do {
        if (esp_http_client_open(client, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (status < 200 || status >= 300) {
            ESP_LOGW(kTag, "GET %s returned HTTP %d", url, status);
            break;
        }

        while (used + 1 < capacity) {
            const int received = esp_http_client_read(
                client, response + used, static_cast<int>(capacity - used - 1));
            if (received < 0) {
                used = 0;
                break;
            }
            if (received == 0) {
                success = used > 0;
                break;
            }
            used += static_cast<std::size_t>(received);
        }
        if (used + 1 >= capacity) {
            ESP_LOGW(kTag, "Response from %s exceeded %u bytes", url,
                     static_cast<unsigned>(capacity));
            success = false;
        }
    } while (false);

    response[used] = '\0';
    if (response_size) *response_size = used;
    ESP_LOGI(kTag, "GET %s -> %s, %u bytes", url, success ? "ok" : "failed",
             static_cast<unsigned>(used));
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return success;
}

const char *json_string(cJSON *object, const char *name) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

void copy_text(char *target, std::size_t capacity, const char *source) {
    if (!target || capacity == 0) return;
    std::snprintf(target, capacity, "%s", source ? source : "");
}

RadioRenderableFn s_renderable;

bool renderable(const char *text) { return !s_renderable || s_renderable(text); }

// The Chinese text when the font can draw it, otherwise the Latin one.
void pick_text(char *target, std::size_t capacity, const char *chinese, const char *latin) {
    if (chinese && chinese[0] && renderable(chinese)) {
        copy_text(target, capacity, chinese);
    } else {
        copy_text(target, capacity, latin && latin[0] ? latin : chinese);
    }
}

bool locate(RadioLocation *location) {
    char *response = static_cast<char *>(std::malloc(2048));
    if (!response) return false;
    bool success = false;
    if (http_get(kLocationUrl, response, 2048, nullptr)) {
        cJSON *root = cJSON_Parse(response);
        if (root) {
            const char *status = json_string(root, "status");
            const char *country = json_string(root, "countryCode");
            const char *region = json_string(root, "regionName");
            const char *city = json_string(root, "city");
            if (std::strcmp(status, "success") == 0 && country[0] &&
                (city[0] || region[0])) {
                copy_text(location->country_code, sizeof(location->country_code), country);
                copy_text(location->region, sizeof(location->region), region);
                copy_text(location->city, sizeof(location->city), city);
                success = true;
            }
            cJSON_Delete(root);
        }
    }
    if (!success && http_get(kBackupLocationUrl, response, 2048, nullptr)) {
        cJSON *root = cJSON_Parse(response);
        if (root) {
            cJSON *ok = cJSON_GetObjectItemCaseSensitive(root, "success");
            const char *country = json_string(root, "country_code");
            const char *region = json_string(root, "region");
            const char *city = json_string(root, "city");
            if (cJSON_IsTrue(ok) && country[0] && (city[0] || region[0])) {
                copy_text(location->country_code, sizeof(location->country_code), country);
                copy_text(location->region, sizeof(location->region), region);
                copy_text(location->city, sizeof(location->city), city);
                success = true;
            }
            cJSON_Delete(root);
        }
    }
    std::free(response);
    return success;
}

void url_encode(const char *source, char *target, std::size_t capacity) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::size_t out = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(source);
         p && *p && out + 1 < capacity; ++p) {
        const unsigned char value = *p;
        if (std::isalnum(value) || value == '-' || value == '_' || value == '.' ||
            value == '~') {
            target[out++] = static_cast<char>(value);
        } else if (out + 3 < capacity) {
            target[out++] = '%';
            target[out++] = kHex[value >> 4];
            target[out++] = kHex[value & 0x0F];
        } else {
            break;
        }
    }
    target[out] = '\0';
}

const char *translated_city(const RadioLocation &location) {
    if (std::strcmp(location.country_code, "CN") != 0) return location.city;
    for (const auto &entry : kChineseCities) {
        if (std::strcmp(entry.english, location.city) == 0) return entry.chinese;
    }
    return nullptr;
}

void normalize_stream_url(const char *source, char *target, std::size_t capacity) {
    // This public Icecast host also serves its MP3 stream over plain HTTP.
    // Avoiding TLS leaves enough internal RAM for the MP3 decoder on ESP32-C3.
    if (starts_with(source, "https://") &&
        contains(source, "radio.nitro-server.uk/")) {
        std::snprintf(target, capacity, "http://%s", source + 8);
    } else if (starts_with(source, "https://") &&
        (contains(source, ".qtfm.cn/") || contains(source, ".qingting.fm/"))) {
        std::snprintf(target, capacity, "http://%s", source + 8);
    } else {
        copy_text(target, capacity, source);
    }
}

void stream_identity(const char *url, char *target, std::size_t capacity) {
    target[0] = '\0';
    if (!url || !target || capacity == 0) return;
    const bool qingting =
        contains(url, "qtfm.cn/") || contains(url, "qingting.fm/");
    const char *live = qingting ? std::strstr(url, "/live/") : nullptr;
    if (live) {
        const char *id = live + 6;
        const char *end = id;
        while (*end >= '0' && *end <= '9') ++end;
        if (end > id) {
            std::snprintf(target, capacity, "qingting:%.*s",
                          static_cast<int>(end - id), id);
            return;
        }
    }
    copy_text(target, capacity, url);
}

bool same_station_name(const char *a, const char *b) {
    return a && b && std::strcmp(a, b) == 0;
}

bool duplicate_station(const RadioStation *stations, std::size_t count,
                       const RadioStation &candidate) {
    char candidate_stream[192] = {};
    stream_identity(candidate.url, candidate_stream, sizeof(candidate_stream));
    for (std::size_t i = 0; i < count; ++i) {
        char existing_stream[192] = {};
        stream_identity(stations[i].url, existing_stream, sizeof(existing_stream));
        if (candidate_stream[0] &&
            std::strcmp(candidate_stream, existing_stream) == 0) {
            return true;
        }
        if (same_station_name(candidate.name, stations[i].name)) return true;
        // One FM frequency carries one station in a city. This also catches
        // aliases whose wording differs but resolve to the same transmitter.
        if (candidate.frequency_decihz && stations[i].frequency_decihz &&
            candidate.frequency_decihz == stations[i].frequency_decihz) {
            return true;
        }
    }
    return false;
}

bool ascii_only(const char *text) {
    if (!text || !text[0]) return false;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
        if (*p >= 0x80) return false;
    }
    return true;
}

// Chinese FM broadcasting occupies 87.0-108.0 MHz. Station names in the public
// directory spell the frequency in three ways, so they are tried in order of
// confidence: an explicit "FM" prefix, a bare decimal such as "94.7", and
// finally a compact form such as "971" that stations use as a brand.
constexpr uint16_t kBandLowDecihz = 870;
constexpr uint16_t kBandHighDecihz = 1080;

bool in_band(long decihz) {
    return decihz >= kBandLowDecihz && decihz <= kBandHighDecihz;
}

bool is_digit(char value) { return value >= '0' && value <= '9'; }

long read_decihz(const char *text, std::size_t *consumed) {
    std::size_t whole = 0;
    while (is_digit(text[whole])) ++whole;
    if (whole < 2 || whole > 4) return 0;

    long value = 0;
    for (std::size_t i = 0; i < whole; ++i) value = value * 10 + (text[i] - '0');

    if (text[whole] == '.' && is_digit(text[whole + 1])) {
        if (whole > 3 || is_digit(text[whole + 2])) return 0;
        *consumed = whole + 2;
        return value * 10 + (text[whole + 1] - '0');
    }

    *consumed = whole;
    if (whole == 2) return value * 10;
    if (in_band(value)) return value;
    if (in_band(value * 10)) return value * 10;
    return value;
}

uint16_t parse_frequency(const char *name) {
    if (!name) return 0;

    for (const char *p = name; p[0] && p[1]; ++p) {
        if ((p[0] != 'F' && p[0] != 'f') || (p[1] != 'M' && p[1] != 'm')) continue;
        const char *digits = p + 2;
        while (*digits == ' ') ++digits;
        std::size_t consumed = 0;
        const long value = read_decihz(digits, &consumed);
        if (in_band(value)) return static_cast<uint16_t>(value);
    }

    for (const char *p = name; *p; ++p) {
        if (!is_digit(*p)) continue;
        const bool starts_run = p == name || (!is_digit(p[-1]) && p[-1] != '.');
        if (!starts_run) continue;
        std::size_t consumed = 0;
        const long value = read_decihz(p, &consumed);
        if (consumed && in_band(value)) return static_cast<uint16_t>(value);
        while (is_digit(p[1])) ++p;
    }
    return 0;
}

void make_display_name(char *target, std::size_t capacity, const char *raw_name,
                       const char *city, std::size_t sequence) {
    struct SimplifiedPair {
        const char *traditional;
        const char *simplified;
    };
    static constexpr SimplifiedPair kSimplifiedNames[] = {
        // Universal character names keep traditional forms out of the rendered
        // font subset; only these simplified output glyphs are needed.
        {"\u9e97", "\u4e3d"}, {"\u5ee3", "\u5e7f"},
        {"\u96fb", "\u7535"}, {"\u81fa", "\u53f0"},
        {"\u8072", "\u58f0"}, {"\u983b", "\u9891"},
        {"\u805e", "\u95fb"}, {"\u6a02", "\u4e50"},
        {"\u5287", "\u5267"}, {"\u6232", "\u620f"},
        {"\u83ef", "\u534e"}, {"\u570b", "\u56fd"},
        {"\u6771", "\u4e1c"}, {"\u8eca", "\u8f66"},
        {"\u8a9e", "\u8bed"}, {"\u7d93", "\u7ecf"},
        {"\u6fdf", "\u6d4e"}, {"\u85dd", "\u827a"},
        {"\u8996", "\u89c6"}, {"\u807d", "\u542c"},
        {"\u9109", "\u4e61"}, {"\u89aa", "\u4eb2"},
        {"\u6642", "\u65f6"}, {"\u7db2", "\u7f51"},
        {"\u7063", "\u6e7e"},
    };
    char cleaned[96] = {};
    std::size_t used = 0;
    for (const unsigned char *p =
             reinterpret_cast<const unsigned char *>(raw_name ? raw_name : "");
         *p && used + 1 < sizeof(cleaned);) {
        const bool fm = (p[0] == 'F' || p[0] == 'f') &&
                        (p[1] == 'M' || p[1] == 'm');
        const bool am = (p[0] == 'A' || p[0] == 'a') &&
                        (p[1] == 'M' || p[1] == 'm');
        if (fm || am) {
            const unsigned char *digits = p + 2;
            while (*digits == ' ') ++digits;
            if (*digits >= '0' && *digits <= '9') {
                p = digits;
                while ((*p >= '0' && *p <= '9') || *p == '.') ++p;
                while (*p == ' ') ++p;
                continue;
            }
        }
        bool converted = false;
        for (const auto &pair : kSimplifiedNames) {
            const std::size_t traditional_length = std::strlen(pair.traditional);
            const std::size_t simplified_length = std::strlen(pair.simplified);
            if (std::strncmp(reinterpret_cast<const char *>(p),
                             pair.traditional, traditional_length) != 0) {
                continue;
            }
            if (used + simplified_length + 1 > sizeof(cleaned)) {
                p += traditional_length;
                converted = true;
                break;
            }
            std::memcpy(cleaned + used, pair.simplified, simplified_length);
            used += simplified_length;
            p += traditional_length;
            converted = true;
            break;
        }
        if (converted) continue;
        if (*p == ' ' && used && cleaned[used - 1] == ' ') {
            ++p;
            continue;
        }
        cleaned[used++] = static_cast<char>(*p++);
    }
    while (used && (cleaned[used - 1] == ' ' || cleaned[used - 1] == '-' ||
                    cleaned[used - 1] == '|')) {
        --used;
    }
    cleaned[used] = '\0';
    // Directory aliases frequently add "台" after the channel type or repeat
    // "网络直播" even though the UI already says this is an online station.
    const char *drop = nullptr;
    const char *broadcast_suffix = nullptr;
    for (const char *suffix : {"广播电台", "广播台"}) {
        const std::size_t length = std::strlen(suffix);
        if (used > length &&
            std::strcmp(cleaned + used - length, suffix) == 0) {
            broadcast_suffix = suffix;
            break;
        }
    }
    if (!broadcast_suffix) {
        for (const char *suffix : {"在线收听", "网络直播"}) {
            const std::size_t length = std::strlen(suffix);
            if (used > length &&
                std::strcmp(cleaned + used - length, suffix) == 0) {
                drop = suffix;
                break;
            }
        }
    }
    if (broadcast_suffix) {
        used -= std::strlen(broadcast_suffix);
        const char radio[] = "广播";
        const std::size_t keep =
            std::min(sizeof(radio) - 1, sizeof(cleaned) - used - 1);
        std::memcpy(cleaned + used, radio, keep);
        used += keep;
        cleaned[used] = '\0';
    } else if (drop) {
        used -= std::strlen(drop);
        cleaned[used] = '\0';
    }
    while (used && cleaned[used - 1] == ' ') cleaned[--used] = '\0';

    std::size_t character_count = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(cleaned);
         *p; ++p) {
        if ((*p & 0xc0) != 0x80) ++character_count;
    }
    char *first_space = std::strchr(cleaned, ' ');
    if (first_space && character_count > 12) {
        *first_space = '\0';
        // Keep compact Latin brands such as "MY FM..." intact.
        if (!cleaned[0] || (ascii_only(cleaned) && std::strlen(cleaned) <= 3)) {
            *first_space = ' ';
        }
    }
    used = std::strlen(cleaned);

    const char *display_name = cleaned[0] ? cleaned : raw_name;
    if (ascii_only(display_name)) {
        copy_text(target, capacity, display_name);
        return;
    }

    // Preserve the meaningful part of the directory name whenever the current
    // font can draw it. Frequency metadata is already shown on the dial.
    if (renderable(display_name)) {
        copy_text(target, capacity, display_name);
        return;
    }

    // The station's own name cannot be spelled with the display glyphs, so use
    // a compact place-based label instead.
    copy_text(target, capacity, city);
    used = std::strlen(target);
    if (used + 1 < capacity) {
        const char *suffix = "电台 ";
        copy_text(target + used, capacity - used, suffix);
        used = std::strlen(target);
        if (used + 1 < capacity) {
            std::snprintf(target + used, capacity - used, "%u",
                          static_cast<unsigned>(sequence + 1));
        }
    }
}

// Adds the station described by one directory entry, if it is playable.
void accept_station(cJSON *entry, const RadioLocation &location, RadioStation *stations,
                    std::size_t capacity, std::size_t *count) {
    if (*count >= capacity || !cJSON_IsObject(entry)) return;
    const char *raw_name = json_string(entry, "name");
    const char *raw_url = json_string(entry, "url_resolved");
    if (!raw_url[0]) raw_url = json_string(entry, "url");
    cJSON *last_ok = cJSON_GetObjectItemCaseSensitive(entry, "lastcheckok");
    cJSON *hls = cJSON_GetObjectItemCaseSensitive(entry, "hls");
    if (!raw_name[0] || !raw_url[0] || !contains(raw_url, ".mp3") ||
        (cJSON_IsNumber(last_ok) && last_ok->valueint != 1) ||
        (cJSON_IsNumber(hls) && hls->valueint != 0)) {
        return;
    }

    RadioStation candidate = {};
    normalize_stream_url(raw_url, candidate.url, sizeof(candidate.url));
    if ((!starts_with(candidate.url, "http://") &&
         !starts_with(candidate.url, "https://"))) {
        return;
    }
    const char *city = location.place[0]  ? location.place
                       : location.city[0] ? location.city
                                          : location.region;
    const char *frequency_city =
        location.city[0] ? location.city : location.region;
    if (!radio_frequency_lookup(frequency_city, raw_name,
                                &candidate.frequency_decihz)) {
        candidate.frequency_decihz = parse_frequency(raw_name);
        if (!candidate.frequency_decihz) {
            candidate.frequency_decihz =
                parse_frequency(json_string(entry, "tags"));
        }
    }
    make_display_name(candidate.name, sizeof(candidate.name), raw_name, city, *count);
    copy_text(candidate.description, sizeof(candidate.description), "城市电台 · 网络直播");
    if (duplicate_station(stations, *count, candidate)) return;
    stations[(*count)++] = candidate;
}

// Walks the response's top-level array one object at a time and parses each on
// its own. Parsing the whole array builds a tree about four times the size of
// the text (47 KB for a 12 KB response), which does not fit beside the scene
// and the response buffer on a chip with no external RAM; one entry is ~4 KB.
std::size_t parse_stations(const char *json, const RadioLocation &location,
                           RadioStation *stations, std::size_t capacity) {
    const char *p = json;
    while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
    if (*p != '[') return 0;
    ++p;

    std::size_t count = 0;
    while (*p && count < capacity) {
        while (*p == ',' || *p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') ++p;
        if (*p != '{') break;  // ']' or something malformed: either way, done
        const char *start = p;
        int depth = 0;
        bool in_string = false;
        for (; *p; ++p) {
            if (in_string) {
                if (*p == '\\' && p[1]) ++p;
                else if (*p == '"') in_string = false;
            } else if (*p == '"') {
                in_string = true;
            } else if (*p == '{') {
                ++depth;
            } else if (*p == '}' && --depth == 0) {
                ++p;
                break;
            }
        }
        if (depth != 0) break;  // truncated response
        cJSON *entry = cJSON_ParseWithLength(start, static_cast<std::size_t>(p - start));
        accept_station(entry, location, stations, capacity, &count);
        cJSON_Delete(entry);
    }
    radio_frequency_enrich_online(stations, count, parse_frequency);
    // Put known FM frequencies first in ascending order. Unknown entries keep
    // their directory order after the tuned stations instead of inventing a
    // frequency for them.
    const auto precedes = [](const RadioStation &a, const RadioStation &b) {
        const bool a_known = a.frequency_decihz != 0;
        const bool b_known = b.frequency_decihz != 0;
        if (a_known != b_known) return a_known;
        if (!a_known) return false;
        return a.frequency_decihz < b.frequency_decihz;
    };
    for (std::size_t i = 1; i < count; ++i) {
        const RadioStation current = stations[i];
        std::size_t j = i;
        while (j > 0 && precedes(current, stations[j - 1])) {
            stations[j] = stations[j - 1];
            --j;
        }
        stations[j] = current;
    }
    // Online enrichment can reveal that two directory aliases use the same
    // transmitter even when their names and stream URLs differ.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < count; ++i) {
        if (!duplicate_station(stations, kept, stations[i])) {
            stations[kept++] = stations[i];
        }
    }
    return kept;
}


// ── pinyin search ──────────────────────────────────────────

// Words radio stations are named with, in pinyin. The directory matches on the
// Chinese name only ("beijing" finds nothing, the Chinese for it finds six
// stations), so what the listener types is turned into Chinese first. Entries
// are matched longest first, so "guangbo" wins over "guang" + "bo".
struct PinyinWord {
    const char *pinyin;
    const char *chinese;
};

constexpr PinyinWord kPinyinWords[] = {
    // cities
    {"beijing", "北京"},   {"shanghai", "上海"},  {"guangzhou", "广州"},
    {"shenzhen", "深圳"},  {"changsha", "长沙"},  {"hangzhou", "杭州"},
    {"chengdu", "成都"},   {"tianjin", "天津"},   {"chongqing", "重庆"},
    {"nanjing", "南京"},   {"suzhou", "苏州"},    {"wuhan", "武汉"},
    {"xian", "西安"},      {"zhengzhou", "郑州"}, {"jinan", "济南"},
    {"qingdao", "青岛"},   {"shenyang", "沈阳"},  {"dalian", "大连"},
    {"harbin", "哈尔滨"},  {"kunming", "昆明"},   {"fuzhou", "福州"},
    {"xiamen", "厦门"},    {"hefei", "合肥"},     {"nanchang", "南昌"},
    {"guiyang", "贵阳"},   {"nanning", "南宁"},   {"haikou", "海口"},
    {"lanzhou", "兰州"},   {"taiyuan", "太原"},   {"changchun", "长春"},
    {"ningbo", "宁波"},    {"wuxi", "无锡"},      {"dongguan", "东莞"},
    {"foshan", "佛山"},    {"zhuhai", "珠海"},    {"huizhou", "惠州"},
    {"zhongshan", "中山"}, {"shantou", "汕头"},   {"hunan", "湖南"},
    {"guangdong", "广东"}, {"zhejiang", "浙江"},  {"jiangsu", "江苏"},
    {"sichuan", "四川"},   {"hubei", "湖北"},     {"henan", "河南"},
    {"shandong", "山东"},  {"fujian", "福建"},    {"yunnan", "云南"},
    {"liaoning", "辽宁"},  {"shaanxi", "陕西"},   {"shanxi", "山西"},
    {"hebei", "河北"},     {"anhui", "安徽"},     {"jiangxi", "江西"},
    {"guangxi", "广西"},   {"hainan", "海南"},    {"gansu", "甘肃"},
    {"xinjiang", "新疆"},  {"xizang", "西藏"},    {"neimenggu", "内蒙古"},
    {"zhongguo", "中国"},  {"liangguang", "两广"}, {"yueyu", "粤语"},
    // programme types
    {"guangbo", "广播"},   {"diantai", "电台"},   {"xinwen", "新闻"},
    {"yinyue", "音乐"},    {"jiaotong", "交通"},  {"caijing", "财经"},
    {"wenyi", "文艺"},     {"zonghe", "综合"},    {"zhisheng", "之声"},
    {"zhongwen", "中文"},  {"jingdian", "经典"},  {"laoge", "老歌"},
    {"liuxing", "流行"},   {"qingsong", "轻松"},  {"qingchen", "清晨"},
    {"shenghuo", "生活"},  {"tiyu", "体育"},      {"gushi", "故事"},
    {"xiqu", "戏曲"},      {"sanwen", "散文"},    {"nongcun", "农村"},
    {"dushi", "都市"},     {"huaiju", "怀旧"},    {"jingji", "经济"},
    {"zixun", "资讯"},     {"pindao", "频道"},    {"zhibo", "直播"},
    {"huayu", "华语"},     {"yingyu", "英语"},    {"xiangsheng", "相声"},
};

bool pinyin_at(const char *text, const PinyinWord **match) {
    std::size_t best = 0;
    for (const auto &word : kPinyinWords) {
        const std::size_t length = std::strlen(word.pinyin);
        if (length > best && std::strncmp(text, word.pinyin, length) == 0) {
            best = length;
            *match = &word;
        }
    }
    return best > 0;
}

// Queries one mirror after another until one answers. `field` may be null to
// ask for the country's most-played stations without narrowing to a place.
std::size_t query_directory(const RadioLocation &location, const char *field,
                            const char *value, RadioStation *stations,
                            std::size_t capacity) {
    char country[16] = {};
    char encoded[192] = {};
    char filter[256] = {};
    url_encode(location.country_code, country, sizeof(country));
    if (field && value && value[0]) {
        url_encode(value, encoded, sizeof(encoded));
        std::snprintf(filter, sizeof(filter), "&%s=%s", field, encoded);
    }

    char *response = static_cast<char *>(std::malloc(kResponseCapacity));
    if (!response) {
        ESP_LOGE(kTag, "No memory for a %u byte directory response",
                 static_cast<unsigned>(kResponseCapacity));
        return 0;
    }
    std::size_t count = 0;
    for (const char *host : kDirectoryHosts) {
        char url[512] = {};
        std::snprintf(url, sizeof(url),
                      "%s?countrycode=%s%s&codec=MP3&hidebroken=true"
                      "&order=clickcount&reverse=true&limit=%u",
                      host, country, filter,
                      static_cast<unsigned>(std::min<std::size_t>(capacity, 8)));
        if (http_get(url, response, kResponseCapacity, nullptr)) {
            count = parse_stations(response, location, stations, capacity);
            break;
        }
    }
    std::free(response);
    return count;
}

}  // namespace

std::size_t radio_catalog_discover(RadioStation *stations, std::size_t capacity,
                                   RadioLocation *location) {
    if (!stations || capacity == 0 || !location) return 0;
    *location = {};
    copy_text(location->country_code, sizeof(location->country_code), kCountry);

    // The country is fixed: this radio plays Chinese stations and its display
    // carries Chinese and Latin glyphs. Geolocation decides which Chinese city
    // to prefer and what place the dial names. The connection can leave the
    // country (a VPN), in which case the reported place has no station in the
    // directory; the dial still names it, and the stations come from all of
    // China.
    RadioLocation located = {};
    std::size_t count = 0;
    char label[sizeof(location->label)] = {};
    if (locate(&located)) {
        ESP_LOGI(kTag, "Detected location: %s, %s, %s", located.city, located.region,
                 located.country_code);
        const char *chinese = std::strcmp(located.country_code, kCountry) == 0
                                  ? translated_city(located)
                                  : nullptr;
        pick_text(label, sizeof(label), chinese,
                  located.city[0] ? located.city : located.region);
        if (chinese && chinese[0]) {
            copy_text(location->city, sizeof(location->city), chinese);
            copy_text(location->region, sizeof(location->region), chinese);
            pick_text(location->place, sizeof(location->place), chinese, located.city);
            count = query_directory(*location, "name", chinese, stations, capacity);
            ESP_LOGI(kTag, "City search '%s' returned %u playable stations", chinese,
                     static_cast<unsigned>(count));
        }
    } else {
        ESP_LOGW(kTag, "Unable to determine network location");
    }

    if (count == 0) {
        *location = nationwide();
        count = query_directory(*location, nullptr, nullptr, stations, capacity);
        ESP_LOGI(kTag, "Nationwide search returned %u playable stations",
                 static_cast<unsigned>(count));
    }
    copy_text(location->label, sizeof(location->label), label[0] ? label : kNationwide);
    return count;
}

std::size_t radio_catalog_discover_city(const char *city, RadioStation *stations,
                                        std::size_t capacity,
                                        RadioLocation *location) {
    if (!city || !city[0] || !stations || capacity == 0 || !location) return 0;
    *location = {};
    copy_text(location->country_code, sizeof(location->country_code), kCountry);
    copy_text(location->city, sizeof(location->city), city);
    copy_text(location->region, sizeof(location->region), city);
    pick_text(location->label, sizeof(location->label), city, city);
    copy_text(location->place, sizeof(location->place), location->label);

    std::size_t count = query_directory(*location, "name", city, stations, capacity);
    ESP_LOGI(kTag, "Manual city search '%s' returned %u playable stations", city,
             static_cast<unsigned>(count));
    if (count == 0) {
        // The city has nothing playable today; nationwide beats an empty dial.
        *location = nationwide();
        count = query_directory(*location, nullptr, nullptr, stations, capacity);
        ESP_LOGI(kTag, "Nationwide search returned %u playable stations",
                 static_cast<unsigned>(count));
    }
    return count;
}

bool radio_catalog_pinyin_to_chinese(const char *pinyin, char *out, std::size_t capacity) {
    if (!pinyin || !out || capacity == 0) return false;
    out[0] = '\0';
    std::size_t used = 0;
    for (const char *p = pinyin; *p;) {
        if (*p == ' ' || *p == '\'') {  // separators typed between syllables
            ++p;
            continue;
        }
        const PinyinWord *word = nullptr;
        if (!pinyin_at(p, &word)) return false;
        const std::size_t chinese = std::strlen(word->chinese);
        if (used + chinese + 1 > capacity) return false;
        std::memcpy(out + used, word->chinese, chinese);
        used += chinese;
        out[used] = '\0';
        p += std::strlen(word->pinyin);
    }
    return used > 0;
}

bool known_city_name(const char *chinese) {
    if (!chinese || !chinese[0]) return false;
    for (unsigned i = 0; i < kRadioCityCount; ++i) {
        if (std::strcmp(kRadioCities[i].name, chinese) == 0) return true;
    }
    return false;
}

void normalize_city_query(const char *input, char *out, std::size_t capacity) {
    if (!out || capacity == 0) return;
    out[0] = '\0';
    if (!input) return;
    char normalized[64] = {};
    std::size_t used = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(input);
         *p && used + 1 < sizeof(normalized); ++p) {
        if (*p == ' ' || *p == '\'' || *p == '-') continue;
        normalized[used++] = static_cast<char>(std::tolower(*p));
    }
    copy_text(out, capacity, normalized);
}

std::size_t radio_catalog_find_cities(const char *query, RadioCityMatch *matches,
                                      std::size_t capacity) {
    if (!query || !matches || capacity == 0) return 0;
    char normalized[64] = {};
    normalize_city_query(query, normalized, sizeof(normalized));
    if (!normalized[0]) return 0;
    std::size_t count = 0;
    for (unsigned i = 0; i < kRadioCityCount && count < capacity; ++i) {
        const RadioCityRecord &city = kRadioCities[i];
        if (std::strcmp(normalized, city.initials) == 0 ||
            std::strcmp(normalized, city.pinyin) == 0 ||
            std::strcmp(query, city.name) == 0) {
            matches[count++] = {city.name, city.province, city.priority};
        }
    }
    std::stable_sort(matches, matches + count,
                     [](const RadioCityMatch &a, const RadioCityMatch &b) {
                         if (a.priority != b.priority) return a.priority > b.priority;
                         return std::strcmp(a.province, b.province) < 0;
                     });
    return count;
}

bool radio_catalog_city_initials(const char *initials, char *out, std::size_t capacity) {
    RadioCityMatch matches[8] = {};
    const std::size_t count = radio_catalog_find_cities(initials, matches, 8);
    if (count != 1) {
        if (out && capacity) out[0] = '\0';
        return false;
    }
    copy_text(out, capacity, matches[0].name);
    return true;
}

bool radio_catalog_unique_city(const char *query, char *out, std::size_t capacity) {
    RadioCityMatch matches[8] = {};
    if (radio_catalog_find_cities(query, matches, 8) != 1) {
        if (out && capacity) out[0] = '\0';
        return false;
    }
    copy_text(out, capacity, matches[0].name);
    return true;
}

std::size_t radio_catalog_search(const char *query, RadioStation *stations,
                                 std::size_t capacity, RadioLocation *location) {
    if (!query || !query[0] || !stations || capacity == 0 || !location) return 0;
    *location = {};
    copy_text(location->country_code, sizeof(location->country_code), kCountry);

    // Lower-case the query: the keyboard has both cases.
    char lower[40] = {};
    char upper[40] = {};
    for (std::size_t i = 0; query[i] && i + 1 < sizeof(lower); ++i) {
        lower[i] = static_cast<char>(std::tolower(static_cast<unsigned char>(query[i])));
        upper[i] = static_cast<char>(std::toupper(static_cast<unsigned char>(query[i])));
    }
    copy_text(location->label, sizeof(location->label), upper);
    copy_text(location->place, sizeof(location->place), upper);

    char city[48] = {};
    RadioCityMatch matches[8] = {};
    const std::size_t match_count = radio_catalog_find_cities(lower, matches, 8);
    if (match_count != 1) {
        ESP_LOGW(kTag, "Search '%s' matched %u cities; selection required",
                 lower, static_cast<unsigned>(match_count));
        return 0;
    }
    copy_text(city, sizeof(city), matches[0].name);

    // A search returns stations from that city only. It does not broaden to
    // programme tags or nationwide results.
    copy_text(location->city, sizeof(location->city), city);
    copy_text(location->region, sizeof(location->region), city);
    copy_text(location->label, sizeof(location->label), city);
    copy_text(location->place, sizeof(location->place), city);
    const std::size_t count = query_directory(*location, "name", city, stations, capacity);
    ESP_LOGI(kTag, "City search '%s' from initials '%s' returned %u stations",
             city, lower, static_cast<unsigned>(count));
    return count;
}

void radio_catalog_set_renderable(RadioRenderableFn check) { s_renderable = check; }

#ifdef CATALOG_TESTING
std::size_t catalog_test_parse(const char *json, const RadioLocation &location,
                               RadioStation *stations, std::size_t capacity) {
    return parse_stations(json, location, stations, capacity);
}

void catalog_test_make_name(char *target, std::size_t capacity, const char *raw_name,
                            const char *city, std::size_t sequence) {
    make_display_name(target, capacity, raw_name, city, sequence);
}
#endif
