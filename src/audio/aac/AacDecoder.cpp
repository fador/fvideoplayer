#include "audio/aac/AacDecoder.hpp"

#include "audio/Simd.hpp"

#include <algorithm>

namespace player::audio::aac {
namespace {

constexpr uint8_t kElementSce = 0;
constexpr uint8_t kElementCpe = 1;
constexpr uint8_t kElementCce = 2;
constexpr uint8_t kElementLfe = 3;
constexpr uint8_t kElementDse = 4;
constexpr uint8_t kElementPce = 5;
constexpr uint8_t kElementFil = 6;
constexpr uint8_t kElementEnd = 7;

// Element sequence per channel configuration (ISO/IEC 14496-3, table 4.5).
// Channels are collected in canonical AAC order and remapped to WAV order.
constexpr uint8_t kLayoutCounts[7] = {0, 1, 1, 2, 3, 3, 4};
constexpr uint8_t kLayoutTypes[7][4] = {
    {kElementSce, kElementSce, kElementSce, kElementSce},
    {kElementSce, kElementSce, kElementSce, kElementSce},  // 1: C
    {kElementCpe, kElementSce, kElementSce, kElementSce},  // 2: L R
    {kElementSce, kElementCpe, kElementSce, kElementSce},  // 3: C L R
    {kElementSce, kElementCpe, kElementSce, kElementSce},  // 4: C L R Cs
    {kElementSce, kElementCpe, kElementCpe, kElementSce},  // 5: C L R Ls Rs
    {kElementSce, kElementCpe, kElementCpe, kElementLfe},  // 6: C L R Ls Rs LFE
};

// Canonical AAC channel order -> WAV channel order.
constexpr uint8_t kChannelOrder[7][8] = {
    {0},
    {0},
    {0, 1},
    {2, 0, 1},
    {2, 0, 1, 3},
    {2, 0, 1, 3, 4},
    {2, 0, 1, 4, 5, 3},
};

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

}  // namespace

bool Decoder::checkSupport(const AudioSpecificConfig& config, std::string& error) const {
    if (config.object_type != static_cast<uint8_t>(ObjectType::Lc)) {
        error = objectTypeName(config.object_type);
        error += " is not supported yet";
        return false;
    }
    if (config.sbr_present || config.ps_present) {
        error = "SBR/PS (HE-AAC) is not supported yet";
        return false;
    }
    if (config.frame_length_short) {
        error = "960-sample frames are not supported yet";
        return false;
    }
    if (config.channel_config < 1 || config.channel_config > 6) {
        error = "channel configurations other than 1..6 are not supported yet";
        return false;
    }
    if (config.sample_rate == 0 || config.channel_count == 0) {
        error = "invalid AudioSpecificConfig";
        return false;
    }
    return true;
}

bool Decoder::configure(const uint8_t* config, size_t size, std::string& error) {
    reset();
    if (!config || size == 0) {
        // No extradata: the first ADTS frame will define the stream.
        return true;
    }

    AudioSpecificConfig parsed;
    if (!parseAudioSpecificConfig(config, size, parsed, &error)) {
        return false;
    }
    if (!checkSupport(parsed, error)) {
        return false;
    }
    config_ = parsed;
    sample_rate_index_ = parsed.sample_rate_index;
    configured_ = true;
    channels_.resize(parsed.channel_count);
    filterbanks_.resize(parsed.channel_count);
    ms_mask_.resize(static_cast<size_t>(kMaxWindows) * kMaxSfb);
    return true;
}

void Decoder::flush() {
    for (Filterbank& filterbank : filterbanks_) {
        filterbank.reset();
    }
    random_state_ = 0x1F2E3D4C;
    pts_ = 0;
    element_index_ = 0;
    element_channels_ = 0;
}

void Decoder::reset() {
    configured_ = false;
    config_ = AudioSpecificConfig{};
    sample_rate_index_ = 0;
    channels_.clear();
    filterbanks_.clear();
    ms_mask_.clear();
    flush();
}

bool Decoder::decodeSingle(BitReader& reader, size_t channel, std::string& error) {
    if (channel >= channels_.size()) {
        return fail(error, "channel element exceeds the output channel count");
    }
    ChannelData& data = channels_[channel];
    if (!decodeIcsPayload(reader, sample_rate_index_, data, nullptr, random_state_, error)) {
        return false;
    }
    if (data.tns_present) {
        applyTns(data.info, data.tns, data.spectrum);
    }
    return true;
}

bool Decoder::decodePair(BitReader& reader, size_t first, std::string& error) {
    if (first + 1 >= channels_.size()) {
        return fail(error, "channel element exceeds the output channel count");
    }
    ChannelData& left = channels_[first];
    ChannelData& right = channels_[first + 1];

    uint32_t common_window = 0;
    if (!reader.read(1, common_window)) {
        return fail(error, "truncated channel pair element");
    }

    bool ms_present = false;
    if (common_window != 0) {
        if (!parseIcsInfo(reader, sample_rate_index_, left.info, &error)) {
            return false;
        }
        right.info = left.info;

        uint32_t ms = 0;
        if (!reader.read(2, ms)) {
            return fail(error, "truncated M/S mask");
        }
        if (ms == 3) {
            return fail(error, "reserved ms_mask_present value");
        }
        ms_present = ms != 0;
        const size_t count = static_cast<size_t>(left.info.num_groups) * left.info.max_sfb;
        std::fill(ms_mask_.begin(), ms_mask_.begin() + count, static_cast<uint8_t>(0));
        if (ms == 1) {
            for (size_t i = 0; i < count; ++i) {
                uint32_t bit = 0;
                if (!reader.read(1, bit)) {
                    return fail(error, "truncated M/S mask");
                }
                ms_mask_[i] = static_cast<uint8_t>(bit);
            }
        } else if (ms == 2) {
            std::fill(ms_mask_.begin(), ms_mask_.begin() + count, static_cast<uint8_t>(1));
        }
    }

    const IcsInfo* shared = common_window != 0 ? &left.info : nullptr;
    if (!decodeIcsPayload(reader, sample_rate_index_, left, shared, random_state_, error) ||
        !decodeIcsPayload(reader, sample_rate_index_, right, shared, random_state_, error)) {
        return false;
    }

    if (common_window != 0 && ms_present) {
        applyMidSide(first);
    }
    applyIntensity(first, common_window != 0 && ms_present);
    if (left.tns_present) {
        applyTns(left.info, left.tns, left.spectrum);
    }
    if (right.tns_present) {
        applyTns(right.info, right.tns, right.spectrum);
    }
    return true;
}

void Decoder::applyMidSide(size_t first) {
    ChannelData& left = channels_[first];
    ChannelData& right = channels_[first + 1];
    const IcsInfo& info = left.info;
    const uint16_t* offsets = info.bands.offsets;

    //    M/S decode:  L = M + S,  R = M - S   (ISO/IEC 14496-3, 4.6.8.1.3)
    unsigned window_index = 0;
    size_t index = 0;
    for (unsigned g = 0; g < info.num_groups; ++g) {
        const unsigned group_length = info.group_length[g];
        for (unsigned sfb = 0; sfb < info.max_sfb; ++sfb, ++index) {
            const uint8_t left_band = left.band_type[g * kSfbStride + sfb];
            const uint8_t right_band = right.band_type[g * kSfbStride + sfb];
            if (ms_mask_[index] == 0 || left_band >= 13 || right_band >= 13) {
                continue;
            }
            const unsigned width = offsets[sfb + 1] - offsets[sfb];
            for (unsigned j = 0; j < group_length; ++j) {
                const unsigned base = (window_index + j) * kWindowSize + offsets[sfb];
                simd::midSide(left.spectrum + base, right.spectrum + base, width);
            }
        }
        window_index += group_length;
    }
}

void Decoder::applyIntensity(size_t first, bool ms_present) {
    ChannelData& left = channels_[first];
    ChannelData& right = channels_[first + 1];
    const IcsInfo& info = right.info;
    const uint16_t* offsets = info.bands.offsets;

    //    Intensity stereo: R = L * scale, scale sign from the band type
    //    (INTENSITY_BT / INTENSITY_BT2) and the M/S mask.
    unsigned window_index = 0;
    size_t index = 0;
    for (unsigned g = 0; g < info.num_groups; ++g) {
        const unsigned group_length = info.group_length[g];
        for (unsigned sfb = 0; sfb < info.max_sfb; ++sfb, ++index) {
            const uint8_t band = right.band_type[g * kSfbStride + sfb];
            if (band != static_cast<uint8_t>(BandType::Intensity) &&
                band != static_cast<uint8_t>(BandType::Intensity2)) {
                continue;
            }
            float sign = band == static_cast<uint8_t>(BandType::Intensity) ? -1.0f : 1.0f;
            if (ms_present && ms_mask_[index] != 0) {
                sign = -sign;
            }
            const float scale = sign * right.intensity_scale[index];
            const unsigned width = offsets[sfb + 1] - offsets[sfb];
            for (unsigned j = 0; j < group_length; ++j) {
                const unsigned base = (window_index + j) * kWindowSize + offsets[sfb];
                simd::scaleCopy(right.spectrum + base, left.spectrum + base, width, scale);
            }
        }
        window_index += group_length;
    }
}

bool Decoder::skipDataStreamElement(BitReader& reader, std::string& error) {
    uint32_t align = 0;
    uint32_t count = 0;
    if (!reader.read(1, align) || !reader.read(8, count)) {
        return fail(error, "truncated data stream element");
    }
    if (count == 255) {
        uint32_t extra = 0;
        if (!reader.read(8, extra)) {
            return fail(error, "truncated data stream element");
        }
        count += extra;
    }
    if (align != 0 && !reader.alignToByte()) {
        return fail(error, "truncated data stream element");
    }
    return reader.skip(static_cast<size_t>(count) * 8) ? true
                                                       : fail(error, "truncated data stream element");
}

bool Decoder::skipFillElement(BitReader& reader, uint32_t count, std::string& error) {
    // The fill element has no instance tag: the 4 bits already read as the
    // element id are the fill count.
    if (count == 15) {
        uint32_t extension = 0;
        if (!reader.read(8, extension)) {
            return fail(error, "truncated fill element");
        }
        count += extension - 1;
    }
    return reader.skip(static_cast<size_t>(count) * 8) ? true
                                                       : fail(error, "truncated fill element");
}

bool Decoder::decodeElement(BitReader& reader, uint8_t type, std::string& error) {
    const uint8_t layout = config_.channel_config;
    if (layout == 0 || layout > 6) {
        return fail(error, "unsupported channel configuration");
    }
    if (element_index_ >= kLayoutCounts[layout]) {
        return fail(error, "too many channel elements");
    }
    if (type != kLayoutTypes[layout][element_index_]) {
        return fail(error, "unexpected channel element type");
    }
    if (type == kElementCpe) {
        if (!decodePair(reader, element_channels_, error)) {
            return false;
        }
        element_channels_ += 2;
    } else {
        if (!decodeSingle(reader, element_channels_, error)) {
            return false;
        }
        element_channels_ += 1;
    }
    ++element_index_;
    return true;
}

Status Decoder::decodeBlock(const uint8_t* data, size_t size, PcmBuffer& pcm,
                            std::string& error) {
    BitReader reader(data, size);
    element_index_ = 0;
    element_channels_ = 0;

    for (;;) {
        uint32_t type = 0;
        if (!reader.read(3, type)) {
            error = "truncated raw data block";
            return Status::Invalid;
        }
        if (type == kElementEnd) {
            break;
        }
        uint32_t tag = 0;
        if (!reader.read(4, tag)) {
            error = "truncated channel element";
            return Status::Invalid;
        }
        if (type == kElementDse) {
            if (!skipDataStreamElement(reader, error)) {
                return Status::Invalid;
            }
            continue;
        }
        if (type == kElementFil) {
            if (!skipFillElement(reader, tag, error)) {
                return Status::Invalid;
            }
            continue;
        }
        if (type == kElementPce) {
            error = "program config elements are not supported yet";
            return Status::Unsupported;
        }
        if (type == kElementCce) {
            error = "coupling channel elements are not supported yet";
            return Status::Unsupported;
        }
        if (!decodeElement(reader, static_cast<uint8_t>(type), error)) {
            return Status::Invalid;
        }
    }

    if (element_channels_ != channels_.size()) {
        error = "channel count does not match the configuration";
        return Status::Invalid;
    }
    assemblePcm(pcm);
    return Status::Ok;
}

void Decoder::assemblePcm(PcmBuffer& pcm) {
    const size_t channel_count = channels_.size();
    const uint8_t* order = kChannelOrder[config_.channel_config];
    float scratch[kFrameSamples] = {};

    pcm.channels = static_cast<uint32_t>(channel_count);
    pcm.sample_rate = config_.sample_rate;
    pcm.pts = pts_;
    pcm.samples.assign(kFrameSamples * channel_count, 0.0f);

    for (size_t canonical = 0; canonical < channel_count; ++canonical) {
        filterbanks_[canonical].synthesize(channels_[canonical].spectrum,
                                           channels_[canonical].info, scratch);
        const size_t output = order[canonical];
        for (size_t i = 0; i < kFrameSamples; ++i) {
            pcm.samples[i * channel_count + output] = scratch[i];
        }
    }
    pts_ += static_cast<int64_t>(kFrameSamples);
}

Status Decoder::decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error) {
    pcm.clear();
    if (!data || size == 0) {
        error = "empty AAC access unit";
        return Status::Invalid;
    }

    const uint8_t* raw = data;
    size_t raw_size = size;

    if (hasAdtsSyncword(data, size)) {
        AdtsHeader header;
        if (!parseAdtsHeader(data, size, header, &error)) {
            return Status::Invalid;
        }
        if (header.raw_blocks != 0) {
            error = "ADTS frames with multiple raw data blocks are not supported";
            return Status::Unsupported;
        }
        if (size < header.frame_length) {
            error = "truncated ADTS frame";
            return Status::Invalid;
        }
        if (!configured_) {
            const AudioSpecificConfig config = configFromAdts(header);
            if (!checkSupport(config, error)) {
                return Status::Unsupported;
            }
            config_ = config;
            sample_rate_index_ = config.sample_rate_index;
            configured_ = true;
            channels_.resize(config.channel_count);
            filterbanks_.resize(config.channel_count);
            ms_mask_.resize(static_cast<size_t>(kMaxWindows) * kMaxSfb);
        }
        raw = data + header.header_length;
        raw_size = header.frame_length - header.header_length;
    } else if (!configured_) {
        error = "no AudioSpecificConfig available";
        return Status::Invalid;
    }

    if (raw_size == 0) {
        error = "empty AAC raw data block";
        return Status::Invalid;
    }
    return decodeBlock(raw, raw_size, pcm, error);
}

}  // namespace player::audio::aac
