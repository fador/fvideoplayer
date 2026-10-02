#pragma once

#include "audio/Pcm.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace player::audio::flac {

enum class Status {
    Ok,
    Unsupported,  // valid FLAC, but a feature this decoder does not implement
    Invalid,      // corrupt or truncated input
};

// From-scratch FLAC decoder, ported from the public-domain dr_flac reference.
// Input is one FLAC frame as stored by a native stream; output is interleaved
// float PCM at the stream's sample rate.
//
// Supported: 1..8 channels, 4..32 bits per sample, fixed block sizes,
// constant/verbatim/fixed/LPC subframes, wasted bits, stereo decorrelation
// (left/side, right/side, mid/side) and both partitioned Rice methods.
class Decoder {
public:
    // config may be empty (the frame header then defines the stream) or a
    // native FLAC metadata header ("fLaC" + STREAMINFO) to learn the
    // parameters before the first frame.
    bool configure(const uint8_t* config, size_t size, std::string& error);

    bool configured() const { return configured_; }
    uint32_t sampleRate() const { return sample_rate_; }
    uint32_t channels() const { return channels_; }
    uint32_t bitsPerSample() const { return bits_; }

    Status decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error);

    // Walks one frame without reconstructing samples and returns its length
    // in bytes (0 on error). The frame parameters are reported when non-null.
    size_t skipFrame(const uint8_t* data, size_t size, uint32_t* block_size = nullptr,
                            uint32_t* sample_rate = nullptr, uint32_t* channels = nullptr,
                            uint32_t* bits_per_sample = nullptr);

    // FLAC frames are independent, so this only clears the reported format.
    void flush();
    void reset();

private:
    bool configured_ = false;
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 0;
    uint32_t bits_ = 0;
    uint32_t block_size_ = 0;
    int64_t pts_ = 0;
};

}  // namespace player::audio::flac
