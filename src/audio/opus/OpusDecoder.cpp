#include "audio/opus/OpusDecoder.hpp"

#include "audio/opus/RangeDecoder.hpp"

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
    if (parsed.channels > 2) {
        error = "more than 2 Opus channels are not supported yet";
        return false;
    }
    head_ = parsed;
    celt_.init(static_cast<int>(head_.channels));
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

    if (info.mode != Mode::Celt) {
        error = "Opus SILK/hybrid decoding not implemented yet";
        return Status::Unsupported;
    }

    const int stream_channels = info.stereo ? 2 : 1;
    if (static_cast<int>(head_.channels) != stream_channels) {
        error = "Opus channel count changes are not supported yet";
        return Status::Unsupported;
    }

    // CELT frames are 2.5 ms << lm, i.e. 120 << lm samples at 48 kHz.
    int lm = 0;
    while (lm <= kCeltMaxLm && (kCeltShortMdct << lm) != info.frame_samples) {
        ++lm;
    }
    if (lm > kCeltMaxLm) {
        error = "invalid Opus CELT frame size";
        return Status::Invalid;
    }

    const uint32_t channels = head_.channels;
    pcm.channels = channels;
    pcm.sample_rate = kOpusSampleRate;
    pcm.pts = pts_;
    pcm.samples.assign(static_cast<size_t>(info.totalSamples()) * channels, 0.0f);

    for (uint8_t f = 0; f < info.frame_count; ++f) {
        const PacketInfo::Frame& frame = info.frames[f];
        RangeDecoder decoder(data + frame.offset, frame.size);
        celt_.decode(decoder, lm, stream_channels,
                     pcm.samples.data() + static_cast<size_t>(f) * info.frame_samples * channels);
    }

    pts_ += info.totalSamples();
    return Status::Ok;
}

void Decoder::flush() {
    pts_ = 0;
}

void Decoder::reset() {
    configured_ = false;
    head_ = OpusHead{};
    pts_ = 0;
    celt_.reset();
}

}  // namespace player::audio::opus
