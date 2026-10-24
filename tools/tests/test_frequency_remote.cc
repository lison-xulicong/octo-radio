#include "radio_frequency_remote_parse.h"

#include <cctype>
#include <cstdio>
#include <cstdlib>

namespace {

int failures = 0;

#define CHECK(condition, ...)                                      \
    do {                                                           \
        if (!(condition)) {                                        \
            std::printf("FAIL %s:%d ", __FILE__, __LINE__);        \
            std::printf(__VA_ARGS__);                              \
            std::printf("\n");                                     \
            ++failures;                                            \
        }                                                          \
    } while (false)

uint16_t parse_fm(const char *text) {
    if (!text) return 0;
    for (const char *p = text; p[0] && p[1]; ++p) {
        if ((p[0] != 'F' && p[0] != 'f') ||
            (p[1] != 'M' && p[1] != 'm')) {
            continue;
        }
        const char *number = p + 2;
        while (*number == ' ') ++number;
        if (!std::isdigit(static_cast<unsigned char>(*number))) continue;
        char *end = nullptr;
        const double mhz = std::strtod(number, &end);
        if (end == number) continue;
        const int decihz = static_cast<int>(mhz * 10.0 + 0.5);
        if (decihz >= 870 && decihz <= 1080) {
            return static_cast<uint16_t>(decihz);
        }
    }
    return 0;
}

}  // namespace

int main() {
    uint32_t channel = 0;
    CHECK(radio_frequency_qingting_channel_id(
              "https://lhttp-hw.qtfm.cn/live/2808/64k.mp3", &channel) &&
              channel == 2808,
          "qtfm live URL did not yield channel 2808");
    CHECK(radio_frequency_qingting_channel_id(
              "http://lhttp.qingting.fm/live/15318146/64k.mp3", &channel) &&
              channel == 15318146,
          "qingting.fm live URL did not yield channel ID");
    CHECK(!radio_frequency_qingting_channel_id(
              "http://example.test/live/2808/64k.mp3", &channel),
          "non-Qingting URL was accepted");
    CHECK(!radio_frequency_qingting_channel_id(
              "http://lhttp.qtfm.cn/radios/2808", &channel),
          "URL without a live ID was accepted");
    CHECK(!radio_frequency_qingting_channel_id(
              "http://lhttp.qtfm.cn/live/0/64k.mp3", &channel),
          "channel zero was accepted");
    CHECK(!radio_frequency_qingting_channel_id(
              "http://lhttp.qtfm.cn/live/99999999999/64k.mp3", &channel),
          "overflowing channel ID was accepted");

    const char *description_frequency =
        R"({"Data":{"title":"苏州新闻广播","description":"FM91.1 总有美好在心（新）间"}})";
    CHECK(radio_frequency_parse_metadata(description_frequency, parse_fm) == 911,
          "description FM frequency was not parsed");

    const char *title_frequency =
        R"({"Data":{"title":"苏州交通经济广播 FM104.8","description":"交通服务"}})";
    CHECK(radio_frequency_parse_metadata(title_frequency, parse_fm) == 1048,
          "title FM frequency fallback was not parsed");

    const char *no_frequency =
        R"({"Data":{"title":"500首华语经典","description":"24小时音乐"}})";
    CHECK(radio_frequency_parse_metadata(no_frequency, parse_fm) == 0,
          "metadata without FM produced a frequency");
    CHECK(radio_frequency_parse_metadata("{not json", parse_fm) == 0,
          "malformed metadata produced a frequency");
    CHECK(radio_frequency_parse_metadata(description_frequency, nullptr) == 0,
          "missing parser was accepted");

    std::printf(failures ? "%d remote-frequency failures\n"
                         : "remote frequency parsing ok\n",
                failures);
    return failures ? 1 : 0;
}
