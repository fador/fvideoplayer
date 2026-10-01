#pragma once

#include "audio/Pcm.hpp"
#include "audio/opus/CeltFrame.hpp"
#include "audio/opus/OpusHead.hpp"
#include "audio/opus/OpusPacket.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace player::audio::opus {

enum class Status {
    Ok,
    Unsupported,  // valid Opus, but a feature this decoder does not implement
    Invalid,      // corrupt or truncated input
};

// From-scratch Opus decoder. Input is one Opus packet as stored by Matroska
// (or MP4); output is interleaved float PCM at 48 kHz.
//
// Current state: OpusHead and packet framing (TOC, codes 0..3, VBR/CBR,
// padding), the range decoder and the CELT layer for CELT-only streams.
// SILK and hybrid packets report Unsupported.
class Decoder {
public:
    // config is an OpusHead as stored in Matroska CodecPrivate / MP4 dOps.
    bool configure(const uint8_t* config, size_t size, std::string& error);

    bool configured() const { return configured_; }
    const OpusHead& head() const { return head_; }
    uint32_t sampleRate() const { return kOpusSampleRate; }
    uint32_t channels() const { return head_.channels; }

    Status decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error);

    void flush();
    void reset();

private:
    OpusHead head_;
    bool configured_ = false;
    int64_t pts_ = 0;
    CeltFrameDecoder celt_;
};

}  // namespace player::audio::opus
