#include "radio_mp3_sync.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <vector>

namespace {

bool expect(bool condition, const char *message) {
    if (condition) return true;
    std::fprintf(stderr, "mp3 sync test failed: %s\n", message);
    return false;
}

}  // namespace

int main() {
    constexpr std::size_t kFrameSize = 208;  // MPEG-1 Layer III, 64 kbps, 44.1 kHz
    constexpr std::uint8_t kHeader[] = {0xff, 0xfb, 0x50, 0xc4};
    constexpr std::size_t kHighFrameSize = 1044;  // MPEG-1 Layer III, 320 kbps
    constexpr std::uint8_t kHighHeader[] = {0xff, 0xfb, 0xe0, 0x00};

    std::vector<std::uint8_t> stream(8, 0x7f);
    for (int frame = 0; frame < 3; ++frame) {
        stream.insert(stream.end(), std::begin(kHeader), std::end(kHeader));
        stream.resize(stream.size() + kFrameSize - sizeof(kHeader), 0);
    }
    if (!expect(octo_audio::find_mp3_frame(stream.data(), stream.size()) == 8,
                "find first complete frame after junk")) {
        return 1;
    }

    std::vector<std::uint8_t> unsynced(512, 0xff);
    if (!expect(octo_audio::find_mp3_frame(unsynced.data(), unsynced.size()) ==
                    unsynced.size(),
                "reject sync-looking bytes without a second frame")) {
        return 1;
    }

    std::size_t frame_size = 0;
    if (!expect(octo_audio::mp3_frame_size(kHeader, sizeof(kHeader), &frame_size),
                "recognize MPEG-1 Layer III header") ||
        !expect(frame_size == kFrameSize, "calculate MP3 frame size")) {
        return 1;
    }
    std::vector<std::uint8_t> high_bitrate(kHighFrameSize, 0);
    std::copy(std::begin(kHighHeader), std::end(kHighHeader),
              high_bitrate.begin());
    high_bitrate.insert(high_bitrate.end(), std::begin(kHighHeader),
                        std::end(kHighHeader));
    if (!expect(octo_audio::find_mp3_frame(high_bitrate.data(),
                                          high_bitrate.size()) == 0,
                "synchronize a 320 kbps MP3 stream")) {
        return 1;
    }
    if (!expect(octo_audio::mp3_frame_size(kHighHeader, sizeof(kHighHeader),
                                           &frame_size),
                "recognize the 320 kbps header") ||
        !expect(frame_size == kHighFrameSize,
                "calculate the 320 kbps frame size")) {
        return 1;
    }
    return 0;
}
