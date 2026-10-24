#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>

constexpr int kDialBandLow = 880;
constexpr int kDialBandHigh = 1080;
constexpr int kDialTrackLeft = 16;
constexpr int kDialTrackRight = 198;
std::size_t s_station_count = 1;
std::size_t s_station_index = 0;
uint16_t s_station_frequency = 0;

int dial_x_for_station() {
    const int span = kDialTrackRight - kDialTrackLeft;
    if (s_station_frequency >= kDialBandLow &&
        s_station_frequency <= kDialBandHigh) {
        const int offset = (s_station_frequency - kDialBandLow) * span /
                           (kDialBandHigh - kDialBandLow);
        return kDialTrackLeft + offset;
    }
    const int count = static_cast<int>(std::max<std::size_t>(s_station_count, 1));
    const int index = static_cast<int>(
        std::min<std::size_t>(s_station_index, s_station_count - 1));
    const int inset = span / 8;
    const int usable = span - inset * 2;
    const int offset = count > 1 ? inset + usable * index / (count - 1)
                                 : inset + usable / 2;
    return kDialTrackLeft + offset;
}

int main() {
    int fail = 0;
    struct Anchor {
        int frequency;
        int x;
    };
    Anchor anchors[] = {
        {880, 16}, {920, 52}, {960, 88},
        {1000, 125}, {1040, 161}, {1080, 198},
    };
    for (const auto &anchor : anchors) {
        s_station_frequency = anchor.frequency;
        const int x = dial_x_for_station();
        if (std::abs(x - anchor.x) > 1) {
            std::printf("FAIL frequency %d -> %d, expected %d\n",
                        anchor.frequency, x, anchor.x);
            ++fail;
        }
    }

    s_station_frequency = 0;
    for (int count = 1; count <= 10; ++count) {
        s_station_count = count;
        int last = -1;
        for (int index = 0; index < count; ++index) {
            s_station_index = index;
            const int x = dial_x_for_station();
            if (x <= kDialTrackLeft + 4 || x >= kDialTrackRight - 4) {
                std::printf("FAIL pinned count=%d index=%d x=%d\n", count, index, x);
                ++fail;
            }
            if (count > 1 && index > 0 && x <= last) {
                std::printf("FAIL not increasing count=%d index=%d x=%d\n",
                            count, index, x);
                ++fail;
            }
            last = x;
        }
    }

    s_station_count = 6;
    std::printf("6-channel fallback positions:");
    for (int index = 0; index < 6; ++index) {
        s_station_index = index;
        std::printf(" %d", dial_x_for_station());
    }
    std::printf("\n");
    std::printf(fail ? "%d failed\n" : "all dial checks passed\n", fail);
    return fail ? 1 : 0;
}
