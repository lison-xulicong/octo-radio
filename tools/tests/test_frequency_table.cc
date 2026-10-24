#include "radio_frequency.h"

#include <cstdio>

int main() {
    int failures = 0;
    struct Case {
        const char *city;
        const char *station;
        uint16_t expected;
    };
    const Case cases[] = {
        {"无锡", "无锡新闻综合广播AM1161", 937},
        {"无锡", "无锡新闻广播 FM93.7", 937},
        {"无锡", "无锡交通广播", 1069},
        {"无锡", "无锡经济广播", 1040},
        {"无锡", "无锡音乐广播", 914},
        {"无锡", "乐动914", 914},
        {"无锡", "无锡梁溪之声", 926},
        {"无锡", "无锡健康广播", 987},
        {"北京", "北京新闻广播", 1006},
        {"北京", "北京音乐广播", 974},
        {"北京", "北京文艺广播", 876},
        {"北京", "北京交通广播", 1039},
        {"上海", "上海新闻广播", 934},
        {"广州", "广州金曲音乐广播", 1027},
        {"广州", "广州新闻资讯广播", 962},
        {"广州", "广州经济交通广播", 1061},
        {"深圳", "深圳新闻广播", 898},
        {"深圳", "深圳音乐广播", 971},
        {"深圳", "深圳生活广播", 942},
        {"长沙", "长沙音乐广播", 886},
        {"长沙", "长沙交通广播", 1061},
        {"长沙", "长沙新闻广播", 1050},
        {"长沙", "长沙品味音乐广播", 1022},
        {"成都", "成都经济广播", 1056},
        {"成都", "成都文化休闲广播", 946},
        {"成都", "成都交通文艺广播", 914},
        {"苏州", "苏州新闻广播", 911},
        {"苏州", "苏州交通经济广播", 1048},
        {"苏州", "苏州都市音乐广播", 1028},
        {"苏州", "苏州生活广播", 965},
        {"苏州", "苏州儿童广播", 957},
        {"苏州", "苏州戏曲广播", 0},
        {"无锡", "不存在的频道", 0},
        {"上海", "无锡音乐广播", 0},
    };

    for (const auto &item : cases) {
        uint16_t frequency = 0;
        const bool found =
            radio_frequency_lookup(item.city, item.station, &frequency);
        if ((item.expected == 0 && found) ||
            (item.expected != 0 &&
             (!found || frequency != item.expected))) {
            std::printf("FAIL %s / %s: found=%d frequency=%u expected=%u\n",
                        item.city, item.station, found, frequency,
                        item.expected);
            ++failures;
        }
    }

    std::printf(failures ? "%d frequency-table failures\n"
                         : "frequency table ok\n",
                failures);
    return failures ? 1 : 0;
}
