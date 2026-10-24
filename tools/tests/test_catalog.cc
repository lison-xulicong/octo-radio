// Host tests for the station catalog: pinyin to Chinese, and the streaming
// parser against real directory responses. The network layer is not involved.
//
//   test_catalog <dir with the saved responses>
#define CATALOG_TESTING 1
#include "cJSON.h"
#include "esp_http_client.h"
#include "radio_catalog.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

std::size_t catalog_test_parse(const char *json, const RadioLocation &location,
                               RadioStation *stations, std::size_t capacity);
void catalog_test_make_name(char *target, std::size_t capacity, const char *raw_name,
                            const char *city, std::size_t sequence);

static int g_fail;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static std::string slurp(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string out; char buf[4096]; size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

static std::string to_chinese(const char *pinyin, bool *ok = nullptr) {
    char out[64];
    const bool good = radio_catalog_pinyin_to_chinese(pinyin, out, sizeof(out));
    if (ok) *ok = good;
    return good ? out : "";
}

int main(int argc, char **argv) {
    // --- pinyin ---------------------------------------------------------
    CHECK(to_chinese("beijing") == "北京", "beijing");
    CHECK(to_chinese("changsha") == "长沙", "changsha");
    CHECK(to_chinese("beijingxinwen") == "北京新闻", "two words: %s", to_chinese("beijingxinwen").c_str());
    CHECK(to_chinese("beijing xinwen") == "北京新闻", "space between words");
    CHECK(to_chinese("yinyueguangbo") == "音乐广播", "guangbo not guang+bo");
    CHECK(to_chinese("xian") == "西安", "xian is one word here, not xi+an");
    CHECK(to_chinese("zhongguozhisheng") == "中国之声", "%s", to_chinese("zhongguozhisheng").c_str());
    bool ok = true;
    to_chinese("qwerty", &ok);
    CHECK(!ok, "nonsense is rejected, not half-converted");
    to_chinese("beijingqwerty", &ok);
    CHECK(!ok, "a known word followed by nonsense is rejected");
    to_chinese("", &ok);
    CHECK(!ok, "empty");
    char tiny[4];
    CHECK(!radio_catalog_pinyin_to_chinese("beijing", tiny, sizeof tiny), "output too small is refused");
    char initials[16];
    CHECK(radio_catalog_city_initials("Wx", initials, sizeof initials) &&
              std::strcmp(initials, "无锡") == 0,
          "mixed-case WX should map to Wuxi");
    CHECK(!radio_catalog_city_initials("news", initials, sizeof initials),
          "non-city search should be rejected");

    RadioCityMatch city_matches[8] = {};
    const std::size_t sz_count =
        radio_catalog_find_cities("sZ", city_matches, 8);
    CHECK(sz_count == 4, "SZ should list all four city matches, got %zu", sz_count);
    bool has_shenzhen = false;
    bool has_suzhou = false;
    for (std::size_t i = 0; i < sz_count; ++i) {
        has_shenzhen |= std::strcmp(city_matches[i].name, "深圳") == 0;
        has_suzhou |= std::strcmp(city_matches[i].name, "苏州") == 0;
    }
    CHECK(has_shenzhen && has_suzhou,
          "SZ city candidates should include Shenzhen and Suzhou");

    CHECK(radio_catalog_find_cities("SUZHOU", city_matches, 8) == 2,
          "SUZHOU should list both Suzhou and Suzhou (Anhui)");
    CHECK(radio_catalog_find_cities("YICHUN", city_matches, 8) == 0,
          "cities without Radio Browser results must not be searchable");
    CHECK(radio_catalog_find_cities("baicheng", city_matches, 8) == 0,
          "Baicheng has no playable Radio Browser MP3 station");
    CHECK(radio_catalog_find_cities("jilin", city_matches, 8) == 1,
          "Jilin City has playable Radio Browser results");
    char unique_city[16];
    CHECK(radio_catalog_unique_city("Wx", unique_city, sizeof unique_city) &&
              std::strcmp(unique_city, "无锡") == 0,
          "WX should uniquely resolve to Wuxi");
    CHECK(!radio_catalog_unique_city("SZ", unique_city, sizeof unique_city),
          "ambiguous SZ must not resolve without a city choice");
    CHECK(radio_catalog_find_cities("杭州", city_matches, 8) == 1,
          "Hangzhou must remain searchable after leaving the default list");
    CHECK(radio_catalog_find_cities("not-a-city", city_matches, 8) == 0,
          "an unknown city should return an empty candidate list");

    // --- display-name fidelity and fallback --------------------------------
    char display[80] = {};
    radio_catalog_set_renderable([](const char *) { return true; });
    catalog_test_make_name(display, sizeof display, "北京新闻广播", "北京", 0);
    CHECK(std::strcmp(display, "北京新闻广播") == 0,
          "renderable original station name was replaced: %s", display);
    catalog_test_make_name(display, sizeof display, "北京新闻广播 FM94.7", "北京", 0);
    CHECK(std::strcmp(display, "北京新闻广播") == 0,
          "frequency metadata was not removed from display name: %s", display);
    catalog_test_make_name(display, sizeof display,
                           "上海新闻广播 FM93.4 AM990", "上海", 0);
    CHECK(std::strcmp(display, "上海新闻广播") == 0,
          "FM/AM metadata cleanup is wrong: %s", display);
    catalog_test_make_name(display, sizeof display,
                           "上海流行音乐广播 动感101 FM101.7", "上海", 0);
    CHECK(std::strcmp(display, "上海流行音乐广播") == 0,
          "multi-word display name cleanup is wrong: %s", display);
    catalog_test_make_name(display, sizeof display, "广州新闻资讯广播台", "广州", 0);
    CHECK(std::strcmp(display, "广州新闻资讯广播") == 0,
          "redundant broadcaster suffix was not removed: %s", display);
    catalog_test_make_name(display, sizeof display, "MY FM全国音乐频道·广州", "广州", 0);
    CHECK(std::strcmp(display, "MY FM全国音乐频道·广州") == 0,
          "FM branding without a numeric frequency was changed: %s", display);
    catalog_test_make_name(
        display, sizeof display,
        "上海麗都廣播電台 1940's Shanghai Vintage Jazz Shidaiqu Radio", "上海", 4);
    CHECK(std::strcmp(display, "上海丽都广播电台") == 0,
          "traditional station name was not converted to simplified Chinese: %s",
          display);

    RadioLocation shanghai = {};
    std::snprintf(shanghai.city, sizeof shanghai.city, "上海");
    std::snprintf(shanghai.region, sizeof shanghai.region, "上海");
    std::snprintf(shanghai.place, sizeof shanghai.place, "上海");
    const char *nitro_station =
        "[{\"name\":\"上海麗都廣播電台 1940's Shanghai Oldtimes Music Radio\","
        "\"url_resolved\":\"https://radio.nitro-server.uk/listen/test/radio.mp3\","
        "\"lastcheckok\":1,\"hls\":0}]";
    RadioStation nitro_result[1] = {};
    CHECK(catalog_test_parse(nitro_station, shanghai, nitro_result, 1) == 1,
          "Nitroradio station did not parse");
    CHECK(std::strstr(nitro_result[0].url, "http://radio.nitro-server.uk/") ==
              nitro_result[0].url,
          "low-memory HTTP stream fallback was not applied: %s", nitro_result[0].url);

    radio_catalog_set_renderable([](const char *) { return false; });
    catalog_test_make_name(display, sizeof display, "北京新闻广播 FM94.7", "北京", 2);
    CHECK(std::strcmp(display, "北京电台 3") == 0,
          "missing glyph fallback is wrong: %s", display);
    catalog_test_make_name(display, sizeof display, "北京新闻广播", "北京", 2);
    CHECK(std::strcmp(display, "北京电台 3") == 0,
          "missing glyph numbered fallback is wrong: %s", display);
    radio_catalog_set_renderable(nullptr);

    // --- streaming parser against real responses ---------------------------
    const std::string dir = argc > 1 ? argv[1] : ".";
    RadioLocation here = {};
    std::snprintf(here.city, sizeof here.city, "北京");
    std::snprintf(here.region, sizeof here.region, "北京");
    std::snprintf(here.place, sizeof here.place, "北京");
    int files = 0;
    for (const char *name : {"bj.json", "q.json", "cn.json", "py.json"}) {
        const std::string json = slurp(dir + "/" + name);
        if (json.empty()) continue;
        ++files;
        RadioStation a[12] = {}, b[12] = {};
        const size_t streamed = catalog_test_parse(json.c_str(), here, a, 12);

        // Reference: parse the whole array at once, as the old code did.
        cJSON *root = cJSON_Parse(json.c_str());
        size_t whole = 0;
        if (root) {
            cJSON *e = nullptr;
            cJSON_ArrayForEach(e, root) {
                cJSON *url = cJSON_GetObjectItemCaseSensitive(e, "url_resolved");
                cJSON *ok_ = cJSON_GetObjectItemCaseSensitive(e, "lastcheckok");
                cJSON *hls = cJSON_GetObjectItemCaseSensitive(e, "hls");
                if (cJSON_IsString(url) && std::strstr(url->valuestring, ".mp3") &&
                    cJSON_IsNumber(ok_) && ok_->valueint == 1 &&
                    cJSON_IsNumber(hls) && hls->valueint == 0) ++whole;
            }
        }
        cJSON_Delete(root);
        CHECK(streamed > 0, "%s: no stations parsed", name);
        CHECK(streamed <= whole, "%s: streamed %zu > playable %zu", name, streamed, whole);
        for (size_t i = 0; i < streamed; ++i) {
            CHECK(a[i].url[0] && a[i].name[0], "%s: station %zu is empty", name, i);
            for (size_t j = 0; j < i; ++j) CHECK(std::strcmp(a[i].url, a[j].url) != 0, "%s: duplicate url", name);
        }
        (void)b;
        std::printf("%-8s %zu stations, first: %s\n", name, streamed, streamed ? a[0].name : "-");
    }
    CHECK(files > 0, "no saved responses found in %s", dir.c_str());

    // --- typed search request ------------------------------------------------
    const std::string search_json = slurp(dir + "/bj.json");
    if (!search_json.empty()) {
        catalog_test_last_url.clear();
        RadioStation ambiguous[8] = {};
        RadioLocation ambiguous_location = {};
        CHECK(radio_catalog_search("SZ", ambiguous, 8, &ambiguous_location) == 0,
              "ambiguous city search should not return a station list");
        CHECK(catalog_test_last_url.empty(),
              "ambiguous city search must not make a directory request");

        catalog_test_response = search_json;
        RadioStation found[8] = {};
        RadioLocation location = {};
        const std::size_t count =
            radio_catalog_search("beijing", found, 8, &location);
        CHECK(count > 0, "beijing search returned no stations");
        CHECK(catalog_test_last_url.find("countrycode=CN") != std::string::npos,
              "search omitted country filter: %s", catalog_test_last_url.c_str());
        CHECK(catalog_test_last_url.find("name=%E5%8C%97%E4%BA%AC") != std::string::npos,
              "pinyin was not converted to an encoded Chinese name: %s",
              catalog_test_last_url.c_str());
        CHECK(std::strcmp(location.label, "北京") == 0,
              "search label should be Beijing, got %s", location.label);

        catalog_test_response = search_json;
        RadioStation initials_found[8] = {};
        location = {};
        const std::size_t initials_count =
            radio_catalog_search("wX", initials_found, 8, &location);
        CHECK(initials_count > 0, "WX city search returned no stations");
        CHECK(catalog_test_last_url.find("name=%E6%97%A0%E9%94%A1") != std::string::npos,
              "WX was not converted to the Wuxi query: %s",
              catalog_test_last_url.c_str());
    }

    // --- offline frequency table integration --------------------------------
    RadioLocation wuxi = {};
    std::snprintf(wuxi.city, sizeof wuxi.city, "无锡");
    std::snprintf(wuxi.region, sizeof wuxi.region, "无锡");
    std::snprintf(wuxi.place, sizeof wuxi.place, "无锡");
    const char *wuxi_station =
        "[{\"name\":\"无锡新闻综合广播AM1161\","
        "\"url_resolved\":\"http://example.test/wuxi.mp3\","
        "\"lastcheckok\":1,\"hls\":0}]";
    RadioStation wuxi_results[4] = {};
    CHECK(catalog_test_parse(wuxi_station, wuxi, wuxi_results, 4) == 1,
          "Wuxi station did not parse");
    CHECK(wuxi_results[0].frequency_decihz == 937,
          "offline frequency lookup was not applied: %u",
          static_cast<unsigned>(wuxi_results[0].frequency_decihz));

    // Known frequencies sort ascending across catalog orders; entries without
    // a verified/declared frequency stay last in their original order.
    RadioLocation guangzhou = {};
    std::snprintf(guangzhou.city, sizeof guangzhou.city, "广州");
    std::snprintf(guangzhou.region, sizeof guangzhou.region, "广州");
    std::snprintf(guangzhou.place, sizeof guangzhou.place, "广州");
    const char *ordered_stations =
        "[{\"name\":\"广州金曲音乐广播\","
        "\"url_resolved\":\"http://example.test/1027.mp3\","
        "\"lastcheckok\":1,\"hls\":0},"
        "{\"name\":\"未知广州电台\","
        "\"url_resolved\":\"http://example.test/unknown.mp3\","
        "\"lastcheckok\":1,\"hls\":0},"
        "{\"name\":\"MY FM全国音乐频道·广州\","
        "\"url_resolved\":\"http://example.test/880.mp3\","
        "\"lastcheckok\":1,\"hls\":0},"
        "{\"name\":\"广州新闻资讯广播\","
        "\"url_resolved\":\"http://example.test/962.mp3\","
        "\"lastcheckok\":1,\"hls\":0}]";
    RadioStation ordered[4] = {};
    CHECK(catalog_test_parse(ordered_stations, guangzhou, ordered, 4) == 4,
          "ordered Guangzhou catalog did not parse");
    CHECK(ordered[0].frequency_decihz == 880 &&
              ordered[1].frequency_decihz == 962 &&
              ordered[2].frequency_decihz == 1027 &&
              ordered[3].frequency_decihz == 0,
          "stations are not sorted by known FM frequency: %u %u %u %u",
          static_cast<unsigned>(ordered[0].frequency_decihz),
          static_cast<unsigned>(ordered[1].frequency_decihz),
          static_cast<unsigned>(ordered[2].frequency_decihz),
          static_cast<unsigned>(ordered[3].frequency_decihz));

    const char *duplicate_stations =
        "[{\"name\":\"广州金曲音乐广播\","
        "\"url_resolved\":\"http://example.test/live/20192/64k.mp3\","
        "\"lastcheckok\":1,\"hls\":0},"
        "{\"name\":\"广州汽车音乐电台金曲音乐广播\","
        "\"url_resolved\":\"http://example.test/live/99998/64k.mp3\","
        "\"lastcheckok\":1,\"hls\":0},"
        "{\"name\":\"广州新闻资讯广播台\","
        "\"url_resolved\":\"http://lhttp.qingting.fm/live/4848/64k.mp3\","
        "\"lastcheckok\":1,\"hls\":0},"
        "{\"name\":\"广州新闻资讯广播\","
        "\"url_resolved\":\"http://example.test/live/99999/64k.mp3\","
        "\"lastcheckok\":1,\"hls\":0},"
        "{\"name\":\"广州经济交通广播电台\","
        "\"url_resolved\":\"http://lhttp.qingting.fm/live/4955/64k.mp3\","
        "\"lastcheckok\":1,\"hls\":0},"
        "{\"name\":\"广州经济交通广播\","
        "\"url_resolved\":\"https://lhttp-hw.qtfm.cn/live/4955/64k.mp3\","
        "\"lastcheckok\":1,\"hls\":0}]";
    RadioStation deduplicated[6] = {};
    const std::size_t deduplicated_count =
        catalog_test_parse(duplicate_stations, guangzhou, deduplicated, 6);
    CHECK(deduplicated_count == 3,
          "station aliases were not deduplicated: %zu", deduplicated_count);
    CHECK(deduplicated[0].frequency_decihz == 962 &&
              deduplicated[1].frequency_decihz == 1027 &&
              deduplicated[2].frequency_decihz == 1061,
          "deduplicated station order is wrong");

    // --- malformed input must not crash ---------------------------------------
    RadioStation s[4] = {};
    CHECK(catalog_test_parse("", here, s, 4) == 0, "empty");
    CHECK(catalog_test_parse("[]", here, s, 4) == 0, "empty array");
    CHECK(catalog_test_parse("{\"a\":1}", here, s, 4) == 0, "object, not array");
    CHECK(catalog_test_parse("[{\"name\":\"x\",\"url\":\"http://a/b.mp3\"", here, s, 4) == 0, "truncated");
    CHECK(catalog_test_parse("[{\"name\":\"a}b\",\"url_resolved\":\"http://a/b.mp3\",\"lastcheckok\":1,\"hls\":0}]", here, s, 4) == 1,
          "a brace inside a string does not end the object");
    CHECK(catalog_test_parse("[{\"name\":\"a\\\"}\",\"url_resolved\":\"http://a/c.mp3\",\"lastcheckok\":1,\"hls\":0}]", here, s, 4) == 1,
          "an escaped quote does not end the string");

    std::printf(g_fail ? "FAILED: %d\n" : "catalog ok\n", g_fail);
    return g_fail ? 1 : 0;
}
