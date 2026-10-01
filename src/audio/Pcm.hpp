#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace player::audio {

// Decoded audio, interleaved 32-bit float samples in [-1, 1]. Sinks convert
// this to whatever format the device wants.
struct PcmBuffer {
    std::vector<float> samples;
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    int64_t pts = 0;  // first sample timestamp in the stream timescale

    size_t frames() const {
        return channels == 0 ? 0 : samples.size() / channels;
    }

    void clear() {
        samples.clear();
    }
};

}  // namespace player::audio
