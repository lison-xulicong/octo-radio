#pragma once

#include <cstddef>
#include <cstdint>

namespace octo_audio {

constexpr std::size_t kMp3MaxFrameSize = 1441;

inline bool mp3_frame_size(const uint8_t *data, std::size_t available,
                           std::size_t *frame_size) {
    if (!data || !frame_size || available < 4 || data[0] != 0xff ||
        (data[1] & 0xe0) != 0xe0) {
        return false;
    }
    const uint8_t version = (data[1] >> 3) & 0x03;
    const uint8_t layer = (data[1] >> 1) & 0x03;
    const uint8_t bitrate_index = data[2] >> 4;
    const uint8_t sample_index = (data[2] >> 2) & 0x03;
    if (version == 1 || layer != 1 || bitrate_index == 0 ||
        bitrate_index == 15 || sample_index == 3) {
        return false;
    }

    static constexpr uint16_t kBitrateMpeg1[16] = {
        0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static constexpr uint16_t kBitrateMpeg2[16] = {
        0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static constexpr uint32_t kSampleRateMpeg1[3] = {44100, 48000, 32000};
    static constexpr uint32_t kSampleRateMpeg2[3] = {22050, 24000, 16000};
    static constexpr uint32_t kSampleRateMpeg25[3] = {11025, 12000, 8000};

    const bool mpeg1 = version == 3;
    const uint32_t bitrate =
        static_cast<uint32_t>((mpeg1 ? kBitrateMpeg1 : kBitrateMpeg2)
                                  [bitrate_index]) *
        1000;
    const uint32_t sample_rate =
        version == 3 ? kSampleRateMpeg1[sample_index]
        : version == 2 ? kSampleRateMpeg2[sample_index]
                       : kSampleRateMpeg25[sample_index];
    const uint32_t padding = (data[2] >> 1) & 0x01;
    const uint32_t size =
        ((mpeg1 ? 144u : 72u) * bitrate) / sample_rate + padding;
    if (size < 24 || size > kMp3MaxFrameSize) return false;
    *frame_size = size;
    return true;
}

// Live MP3 streams can begin in the middle of a frame. Requiring a second
// complete header avoids accepting a random 11-bit sync word as a frame start.
inline std::size_t find_mp3_frame(const uint8_t *data, std::size_t length) {
    if (!data) return length;
    for (std::size_t offset = 0; offset + 4 <= length; ++offset) {
        std::size_t first_size = 0;
        if (!mp3_frame_size(data + offset, length - offset, &first_size)) continue;
        const std::size_t next = offset + first_size;
        std::size_t next_size = 0;
        if (next + 4 <= length &&
            mp3_frame_size(data + next, length - next, &next_size)) {
            return offset;
        }
    }
    return length;
}

}  // namespace octo_audio
