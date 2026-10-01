#include "audio/opus/OpusDecoder.hpp"

namespace player::audio::opus {

bool Decoder::configure(const uint8_t* config, size_t size, std::string& error) {
    reset();
    if (!config || size == 0) {
        error = "no OpusHead available";
        return false;
    }
    OpusHead parsed;
    if (!parseOpusHead(config, size, parsed, &error)) {
        return false;
    }
    if (parsed.channels > 8) {
        error = "more than 8 Opus channels are not supported yet";
        return false;
    }
    head_ = parsed;
    configured_ = true;
    return true;
}

Status Decoder::decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error) {
    pcm.clear();
    if (!configured_) {
        error = "no OpusHead available";
        return Status::Invalid;
    }
    if (!data || size == 0) {
        error = "empty Opus packet";
        return Status::Invalid;
    }

    PacketInfo info;
    if (!parsePacket(data, size, info, &error)) {
        return Status::Invalid;
    }
    if (info.frame_count == 0 || info.totalSamples() == 0) {
        error = "empty Opus packet";
        return Status::Invalid;
    }

    // Next milestone: the SILK and CELT layers (range-coded energy, PVQ/LPC,
    // stereo, and the inverse MDCT) turn the frames into PCM.
    error = "Opus SILK/CELT decoding not implemented yet";
    return Status::Unsupported;
}

void Decoder::flush() {
    pts_ = 0;
}

void Decoder::reset() {
    configured_ = false;
    head_ = OpusHead{};
    pts_ = 0;
}

}  // namespace player::audio::opus
