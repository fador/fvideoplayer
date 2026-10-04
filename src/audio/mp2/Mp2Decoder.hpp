#pragma once

#include "audio/Pcm.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace player::audio::mp2 {

enum class Status {
    Ok,
    Unsupported,  // valid MPEG audio, but not Layer II
    Invalid,      // corrupt or truncated input
};

// From-scratch MPEG-1/2 Layer II (MP2) decoder.
//
// Layer II shares the frame header and the polyphase synthesis filterbank with
// Layer III, but its subband data is coded completely differently: bit
// allocation, scalefactors and grouped samples. Input is one MP2 frame; output
// is interleaved float PCM at the stream's sample rate.
class Decoder {
public:
    // Layer II carries no extradata; an empty config is the only accepted one.
    bool configure(const uint8_t* config, size_t size, std::string& error);

    bool configured() const { return configured_; }
    uint32_t sampleRate() const { return sample_rate_; }
    uint32_t channels() const { return channels_; }

    Status decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error);

    // Clears the synthesis history; used after seeking.
    void flush();

    void reset();

private:
    void resetState();

    bool configured_ = false;
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 0;
    float qmf_state_[15 * 2 * 32] = {};
};

}  // namespace player::audio::mp2
