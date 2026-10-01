#include "audio/opus/OpusDecoder.hpp"

#include "audio/opus/RangeDecoder.hpp"

#include <algorithm>

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
    silk_.init(static_cast<int>(head_.channels));
    pre_skip_remaining_ = head_.pre_skip;
    configured_ = true;
    return true;
}

Status Decoder::decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error) {
    pcm.clear();
    if (!configured_) {
        error = "no OpusHead available";
        return Status::Invalid;
    }

    Status status;
    if (!data || size == 0) {
        status = conceal(pcm, error);
    } else {
        PacketInfo info;
        if (!parsePacket(data, size, info, &error)) {
            return Status::Invalid;
        }
        if (info.frame_count == 0 || info.totalSamples() == 0) {
            error = "empty Opus packet";
            return Status::Invalid;
        }

        if (info.mode == Mode::Silk) {
            status = decodeSilk(data, info, pcm, error);
        } else if (info.mode == Mode::Hybrid) {
            status = decodeHybrid(data, info, pcm, error);
        } else {
            status = decodeCelt(data, info, pcm, error);
        }
    }
    if (status == Status::Ok) {
        // The first pre-skip samples are the decoder ramp-up and are dropped.
        const size_t frames = pcm.samples.size() / pcm.channels;
        const size_t drop = std::min<size_t>(frames, pre_skip_remaining_);
        if (drop > 0) {
            pcm.samples.erase(pcm.samples.begin(),
                              pcm.samples.begin() + static_cast<ptrdiff_t>(drop * pcm.channels));
            pre_skip_remaining_ -= static_cast<uint32_t>(drop);
        }
    }
    return status;
}

Status Decoder::conceal(PcmBuffer& pcm, std::string& error) {
    if (last_mode_ != Mode::Celt) {
        error = "Opus packet loss concealment is only implemented for CELT";
        return Status::Unsupported;
    }

    const uint32_t channels = head_.channels;
    const int frames = kCeltShortMdct << last_lm_;
    pcm.channels = channels;
    pcm.sample_rate = kOpusSampleRate;
    pcm.pts = pts_;
    pcm.samples.assign(static_cast<size_t>(frames) * channels, 0.0f);
    celt_.decodeLost(last_lm_, pcm.samples.data());

    pts_ += frames;
    return Status::Ok;
}

Status Decoder::decodeCelt(const uint8_t* data, const PacketInfo& info, PcmBuffer& pcm,
                           std::string& error) {
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
    last_mode_ = Mode::Celt;
    last_lm_ = lm;
    return Status::Ok;
}

void Decoder::flush() {
    pts_ = 0;
}

void Decoder::reset() {
    configured_ = false;
    head_ = OpusHead{};
    pts_ = 0;
    last_mode_ = Mode::Celt;
    last_lm_ = 0;
    celt_.reset();
    silk_.reset();
}

Status Decoder::decodeSilk(const uint8_t* data, const PacketInfo& info, PcmBuffer& pcm,
                           std::string& error) {
    const int stream_channels = info.stereo ? 2 : 1;
    if (static_cast<int>(head_.channels) != stream_channels) {
        error = "Opus channel count changes are not supported yet";
        return Status::Unsupported;
    }

    int fs_khz = 16;
    if (info.bandwidth == Bandwidth::Narrowband) {
        fs_khz = 8;
    } else if (info.bandwidth == Bandwidth::Mediumband) {
        fs_khz = 12;
    }

    // A 10 ms frame has one SILK frame, longer ones use 20 ms SILK frames.
    const int frame_ms = info.frame_samples == 480 ? 10 : 20;
    const int frames = info.frame_samples / (frame_ms * 48);
    if (frames < 1 || frames > 3) {
        error = "invalid Opus SILK frame size";
        return Status::Invalid;
    }

    const uint32_t channels = head_.channels;
    pcm.channels = channels;
    pcm.sample_rate = kOpusSampleRate;
    pcm.pts = pts_;
    pcm.samples.assign(static_cast<size_t>(info.totalSamples()) * channels, 0.0f);

    const int silk_frame_samples = info.frame_samples / frames;
    for (uint8_t f = 0; f < info.frame_count; ++f) {
        const PacketInfo::Frame& frame = info.frames[f];
        RangeDecoder decoder(data + frame.offset, frame.size);
        float* out = pcm.samples.data() + static_cast<size_t>(f) * info.frame_samples * channels;
        for (int k = 0; k < frames; ++k) {
            silk_.decode(decoder, fs_khz, stream_channels, frames, frame_ms,
                         k == 0 ? silk::SilkDecoder::StartOfPacket::Yes
                                : silk::SilkDecoder::StartOfPacket::No,
                         out);
            out += static_cast<size_t>(silk_frame_samples) * channels;
        }
    }

    pts_ += info.totalSamples();
    last_mode_ = Mode::Silk;
    return Status::Ok;
}

Status Decoder::decodeHybrid(const uint8_t* data, const PacketInfo& info, PcmBuffer& pcm,
                             std::string& error) {
    const int stream_channels = info.stereo ? 2 : 1;
    if (static_cast<int>(head_.channels) != stream_channels) {
        error = "Opus channel count changes are not supported yet";
        return Status::Unsupported;
    }

    // Hybrid packets always use the wideband SILK layer plus CELT from the
    // 8 kHz band up.
    const int frame_ms = info.frame_samples == 480 ? 10 : 20;
    const int frames = info.frame_samples / (frame_ms * 48);
    int lm = 0;
    while (lm <= kCeltMaxLm && (kCeltShortMdct << lm) != info.frame_samples) {
        ++lm;
    }
    if (lm > kCeltMaxLm) {
        error = "invalid Opus hybrid frame size";
        return Status::Invalid;
    }

    const uint32_t channels = head_.channels;
    pcm.channels = channels;
    pcm.sample_rate = kOpusSampleRate;
    pcm.pts = pts_;
    pcm.samples.assign(static_cast<size_t>(info.totalSamples()) * channels, 0.0f);

    const int silk_frame_samples = info.frame_samples / frames;
    std::vector<float> silk_pcm(static_cast<size_t>(silk_frame_samples) * channels);
    for (uint8_t f = 0; f < info.frame_count; ++f) {
        const PacketInfo::Frame& frame = info.frames[f];
        RangeDecoder decoder(data + frame.offset, frame.size);
        float* out = pcm.samples.data() + static_cast<size_t>(f) * info.frame_samples * channels;

        float* silk_out = silk_pcm.data();
        for (int k = 0; k < frames; ++k) {
            silk_.decode(decoder, 16, stream_channels, frames, frame_ms,
                         k == 0 ? silk::SilkDecoder::StartOfPacket::Yes
                                : silk::SilkDecoder::StartOfPacket::No,
                         silk_out);
            silk_out += static_cast<size_t>(silk_frame_samples) * channels;
        }

        // Hybrid packets can carry a redundant CELT frame after the SILK
        // data; the flag and its length come before the CELT frame.
        size_t len = frame.size;
        if (decoder.tell() + 17 + 20 <= static_cast<int>(8 * len)) {
            if (decoder.decodeBitLogp(12)) {
                decoder.decodeBitLogp(1);  // celt_to_silk
                const int redundancy_bytes = static_cast<int>(decoder.decodeUint(256)) + 2;
                len -= static_cast<size_t>(redundancy_bytes);
                if (static_cast<int>(len * 8) < decoder.tell()) {
                    len = 0;
                } else {
                    decoder.reduceSize(static_cast<size_t>(redundancy_bytes));
                }
            }
        }

        // The CELT layer covers up to 12 kHz in superwideband hybrid mode.
        const int end = info.bandwidth == Bandwidth::SuperWideband ? kCeltSuperWideEndBand
                                                                    : kCeltBands;
        celt_.decode(decoder, lm, stream_channels, out, kCeltHybridStartBand, end);
        for (size_t i = 0; i < silk_pcm.size(); ++i) {
            out[i] += silk_pcm[i];
        }
    }

    pts_ += info.totalSamples();
    last_mode_ = Mode::Hybrid;
    last_lm_ = lm;
    return Status::Ok;
}

}  // namespace player::audio::opus
