#pragma once

#include "audio/Pcm.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace player::audio::mp3 {

enum class Status {
    Ok,
    Unsupported,  // valid MPEG audio, but not Layer III (MP3)
    Invalid,      // corrupt or truncated input
};

// From-scratch MPEG-1/2/2.5 Layer III (MP3) decoder, ported from the
// public-domain minimp3 reference (scalar path). Input is one MP3 frame;
// output is interleaved float PCM at the stream's sample rate.
class Decoder {
public:
    // MP3 has no extradata; an empty config is the only accepted value.
    bool configure(const uint8_t* config, size_t size, std::string& error);

    bool configured() const { return configured_; }
    uint32_t sampleRate() const { return sample_rate_; }
    uint32_t channels() const { return channels_; }

    Status decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error);

    // Clears the inter-frame state (bit reservoir, overlap, synthesis
    // history); used after seeking.
    void flush();

    void reset();

private:
    void resetState();

    bool configured_ = false;
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 0;
    int64_t pts_ = 0;
    uint8_t header_[4] = {};
    int free_format_bytes_ = 0;
    int reservoir_ = 0;
    uint8_t reservoir_buf_[511] = {};
    float mdct_overlap_[2][9 * 32] = {};
    float qmf_state_[15 * 2 * 32] = {};
};

}  // namespace player::audio::mp3
