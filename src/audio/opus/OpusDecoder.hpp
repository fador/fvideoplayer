#pragma once

#include "audio/Pcm.hpp"
#include "audio/opus/CeltFrame.hpp"
#include "audio/opus/OpusHead.hpp"
#include "audio/opus/OpusPacket.hpp"
#include "audio/opus/SilkDecoder.hpp"

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
// padding), the range decoder, the CELT layer for CELT-only streams and the
// SILK layer for SILK-only streams. Hybrid packets report Unsupported.
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
    Status decodeSilk(const uint8_t* data, const PacketInfo& info, PcmBuffer& pcm,
                      std::string& error);
    Status decodeHybrid(const uint8_t* data, const PacketInfo& info, PcmBuffer& pcm,
                        std::string& error);
    Status decodeCelt(const uint8_t* data, const PacketInfo& info, PcmBuffer& pcm,
                      std::string& error);

    // Conceals a lost packet (decode with no data) using the last decoded
    // frame size and mode.
    Status conceal(PcmBuffer& pcm, std::string& error);

    OpusHead head_;
    bool configured_ = false;
    int64_t pts_ = 0;
    uint32_t pre_skip_remaining_ = 0;
    Mode last_mode_ = Mode::Celt;
    int last_lm_ = 0;
    int last_stream_channels_ = 0;
    int last_frame_samples_ = 0;
    int last_silk_frames_ = 0;
    int last_frame_ms_ = 0;
    int last_fs_khz_ = 0;
    Bandwidth last_bandwidth_ = Bandwidth::Fullband;
    CeltFrameDecoder celt_;
    silk::SilkDecoder silk_;
};

}  // namespace player::audio::opus
