#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace player::audio::opus {

// Opus always decodes to 48 kHz; the container's sample rate is the original
// input rate and is only informational.
constexpr uint32_t kOpusSampleRate = 48000;
constexpr size_t kMaxChannels = 255;

// OpusHead, the identification header from RFC 7845 (Ogg/Matroska extradata).
struct OpusHead {
    uint8_t version = 0;
    uint8_t channels = 0;
    uint16_t pre_skip = 0;       // samples to discard at 48 kHz
    uint32_t input_sample_rate = 0;
    int16_t output_gain = 0;     // Q7.8 dB
    uint8_t mapping_family = 0;
    uint8_t stream_count = 1;
    uint8_t coupled_count = 0;
    uint8_t channel_mapping[kMaxChannels] = {};
};

bool parseOpusHead(const uint8_t* data, size_t size, OpusHead& head, std::string* error);

// Linear gain implied by output_gain.
float outputGainFactor(int16_t output_gain);

}  // namespace player::audio::opus
