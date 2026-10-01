#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace player::audio::opus {

enum class Mode : uint8_t {
    Silk,
    Hybrid,
    Celt,
};

enum class Bandwidth : uint8_t {
    Narrowband,
    Mediumband,
    Wideband,
    SuperWideband,
    Fullband,
};

constexpr size_t kMaxFramesPerPacket = 48;

// Parsed TOC byte plus the frame layout of one Opus packet (RFC 6716 3.1/3.2).
struct PacketInfo {
    Mode mode = Mode::Celt;
    Bandwidth bandwidth = Bandwidth::Fullband;
    bool stereo = false;
    uint8_t frame_count = 0;
    uint16_t frame_samples = 0;  // per frame, at 48 kHz
    size_t header_size = 0;      // TOC + packing headers
    struct Frame {
        size_t offset = 0;  // relative to the packet start
        size_t size = 0;
    };
    Frame frames[kMaxFramesPerPacket] = {};

    // Total duration of the packet in samples at 48 kHz.
    uint32_t totalSamples() const {
        return static_cast<uint32_t>(frame_samples) * frame_count;
    }
};

bool parsePacket(const uint8_t* data, size_t size, PacketInfo& info, std::string* error);

// Samples per frame at 48 kHz for a TOC byte (used by the parser and tests).
uint16_t samplesPerFrame(uint8_t toc);

}  // namespace player::audio::opus
