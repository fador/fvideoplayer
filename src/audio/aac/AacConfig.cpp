#include "audio/aac/AacConfig.hpp"

#include "audio/BitReader.hpp"

namespace player::audio::aac {
namespace {

constexpr uint32_t kSampleRates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

constexpr uint16_t kAdtsSyncword = 0xFFF;
constexpr uint16_t kAscSyncExtension = 0x2B7;
constexpr uint8_t kAdtsHeaderBytes = 7;
constexpr uint8_t kAdtsCrcBytes = 2;

bool fail(std::string* error, const char* message) {
    if (error) {
        *error = message;
    }
    return false;
}

// audioObjectType: 5 bits, escaped value 31 extends to 6 more bits.
bool readObjectType(BitReader& reader, uint8_t& object_type) {
    uint32_t value = 0;
    if (!reader.read(5, value)) {
        return false;
    }
    if (value == 31 && !reader.read(6, value)) {
        return false;
    }
    object_type = static_cast<uint8_t>(value);
    return true;
}

// samplingFrequencyIndex: 4 bits, index 15 carries an explicit 24-bit rate.
bool readSampleRate(BitReader& reader, uint32_t& sample_rate, uint8_t& sample_rate_index) {
    uint32_t index = 0;
    if (!reader.read(4, index)) {
        return false;
    }
    if (index == kExplicitSampleRateIndex) {
        if (!reader.read(24, sample_rate)) {
            return false;
        }
        sample_rate_index = nearestSampleRateIndex(sample_rate);
        return sample_rate != kInvalidSampleRate;
    }
    sample_rate = sampleRateForIndex(static_cast<uint8_t>(index));
    sample_rate_index = static_cast<uint8_t>(index);
    return sample_rate != kInvalidSampleRate;
}

}  // namespace

uint32_t sampleRateForIndex(uint8_t index) {
    return index < sizeof(kSampleRates) / sizeof(kSampleRates[0]) ? kSampleRates[index]
                                                                  : kInvalidSampleRate;
}

uint8_t nearestSampleRateIndex(uint32_t sample_rate) {
    uint8_t best = 0;
    uint32_t best_distance = UINT32_MAX;
    for (uint8_t i = 0; i < sizeof(kSampleRates) / sizeof(kSampleRates[0]); ++i) {
        const uint32_t distance =
            sample_rate > kSampleRates[i] ? sample_rate - kSampleRates[i] : kSampleRates[i] - sample_rate;
        if (distance < best_distance) {
            best_distance = distance;
            best = i;
        }
    }
    return best;
}

uint32_t channelsForConfig(uint8_t channel_config) {
    // 0 = defined by a program config element, 7 = 8 channels (7.1).
    if (channel_config == 0 || channel_config > 7) {
        return 0;
    }
    return channel_config == 7 ? 8 : channel_config;
}

const char* objectTypeName(uint8_t object_type) {
    switch (static_cast<ObjectType>(object_type)) {
        case ObjectType::Main:
            return "AAC Main";
        case ObjectType::Lc:
            return "AAC LC";
        case ObjectType::Ssr:
            return "AAC SSR";
        case ObjectType::Ltp:
            return "AAC LTP";
        case ObjectType::Sbr:
            return "HE-AAC";
        case ObjectType::Scalable:
            return "AAC Scalable";
        case ObjectType::ErLc:
            return "ER AAC LC";
        case ObjectType::Ps:
            return "HE-AAC v2";
        default:
            return "AAC (unknown)";
    }
}

bool parseAudioSpecificConfig(const uint8_t* data, size_t size, AudioSpecificConfig& config,
                              std::string* error) {
    if (!data || size < 2) {
        return fail(error, "short AudioSpecificConfig");
    }
    BitReader reader(data, size);

    uint8_t object_type = 0;
    uint32_t sample_rate = 0;
    uint8_t sample_rate_index = 0;
    uint32_t channel_config = 0;
    if (!readObjectType(reader, object_type) ||
        !readSampleRate(reader, sample_rate, sample_rate_index) ||
        !reader.read(4, channel_config)) {
        return fail(error, "truncated AudioSpecificConfig");
    }

    //    ASC layout for AAC LC (ISO/IEC 14496-3)
    //    +--------------------------------------+
    //    | object type        | 5 bits          |
    //    | sampling frequency | 4 (or 24) bits  |
    //    | channel config     | 4 bits          |
    //    | GASpecificConfig   | 3+ bits         |
    //    | sync extension     | optional        |
    //    +--------------------------------------+
    bool sbr_present = false;
    bool ps_present = false;
    uint32_t extension_rate = 0;
    if (object_type == static_cast<uint8_t>(ObjectType::Sbr) ||
        object_type == static_cast<uint8_t>(ObjectType::Ps)) {
        ps_present = object_type == static_cast<uint8_t>(ObjectType::Ps);
        sbr_present = true;
        uint8_t extension_index = 0;
        if (!readSampleRate(reader, extension_rate, extension_index) ||
            !readObjectType(reader, object_type)) {
            return fail(error, "truncated SBR AudioSpecificConfig");
        }
        // ER BSAC carries an extra channel configuration nibble.
        if (object_type == 22 && !reader.skip(4)) {
            return fail(error, "truncated AudioSpecificConfig");
        }
    }

    // GASpecificConfig: frameLengthFlag, dependsOnCoreCoder (+delay), extensionFlag.
    uint32_t frame_length_flag = 0;
    uint32_t depends_on_core = 0;
    uint32_t extension_flag = 0;
    if (!reader.read(1, frame_length_flag) || !reader.read(1, depends_on_core)) {
        return fail(error, "truncated AudioSpecificConfig");
    }
    if (depends_on_core != 0 && !reader.skip(14)) {
        return fail(error, "truncated AudioSpecificConfig");
    }
    if (!reader.read(1, extension_flag)) {
        return fail(error, "truncated AudioSpecificConfig");
    }
    if (channel_config == 0) {
        return fail(error, "channel configuration 0 (program config element) not supported");
    }

    // Sync extension: announces SBR/PS for streams signalled as plain AAC LC.
    uint32_t sync = 0;
    if (reader.bitsLeft() >= 16 && reader.read(11, sync) && sync == kAscSyncExtension) {
        uint8_t extension_object_type = 0;
        if (readObjectType(reader, extension_object_type)) {
            if (extension_object_type == static_cast<uint8_t>(ObjectType::Sbr)) {
                uint32_t flag = 0;
                if (reader.read(1, flag)) {
                    sbr_present = flag != 0;
                }
            } else if (extension_object_type == static_cast<uint8_t>(ObjectType::Ps)) {
                uint32_t flag = 0;
                if (reader.read(1, flag)) {
                    ps_present = flag != 0;
                }
            }
        }
    }

    config.object_type = object_type;
    config.sample_rate = sample_rate;
    config.sample_rate_index = sample_rate_index;
    config.channel_config = static_cast<uint8_t>(channel_config);
    config.channel_count = channelsForConfig(config.channel_config);
    config.extension_sample_rate = extension_rate;
    config.frame_length_short = frame_length_flag != 0;
    config.sbr_present = sbr_present;
    config.ps_present = ps_present;
    return true;
}

bool parseAdtsHeader(const uint8_t* data, size_t size, AdtsHeader& header, std::string* error) {
    if (!data || size < kAdtsHeaderBytes) {
        return fail(error, "short ADTS header");
    }
    if (!hasAdtsSyncword(data, size)) {
        return fail(error, "bad ADTS syncword");
    }

    //    ADTS header (7 bytes, +2 when a CRC follows)
    //    +-----------------------------------------------+
    //    | syncword 0xFFF        | 12 bits               |
    //    | ID layer protection   | 4 bits                |
    //    | profile freq_index    | 2 + 4 bits            |
    //    | channel_config        | 3 bits                |
    //    | frame_length          | 13 bits               |
    //    | buffer fullness       | 11 bits               |
    //    | raw data blocks       | 2 bits                |
    //    +-----------------------------------------------+
    const bool protection_absent = (data[1] & 0x01) != 0;
    const uint8_t header_length =
        static_cast<uint8_t>(protection_absent ? kAdtsHeaderBytes : kAdtsHeaderBytes + kAdtsCrcBytes);
    if (size < header_length) {
        return fail(error, "short ADTS header with CRC");
    }

    const uint8_t profile = (data[2] >> 6) & 0x03;
    const uint8_t freq_index = (data[2] >> 2) & 0x0F;
    const uint32_t sample_rate = sampleRateForIndex(freq_index);
    if (sample_rate == kInvalidSampleRate) {
        return fail(error, "invalid ADTS sampling frequency index");
    }
    const uint32_t frame_length =
        (static_cast<uint32_t>(data[3] & 0x03) << 11) | (static_cast<uint32_t>(data[4]) << 3) |
        (static_cast<uint32_t>(data[5]) >> 5);
    if (frame_length < header_length) {
        return fail(error, "invalid ADTS frame length");
    }

    header.object_type = static_cast<uint8_t>(profile + 1);
    header.sample_rate = sample_rate;
    header.sample_rate_index = freq_index;
    header.channel_config = static_cast<uint8_t>(((data[2] & 0x01) << 2) | (data[3] >> 6));
    header.frame_length = static_cast<uint16_t>(frame_length);
    header.header_length = header_length;
    header.raw_blocks = static_cast<uint8_t>(data[6] & 0x03);
    return true;
}

AudioSpecificConfig configFromAdts(const AdtsHeader& header) {
    AudioSpecificConfig config;
    config.object_type = header.object_type;
    config.sample_rate = header.sample_rate;
    config.sample_rate_index = header.sample_rate_index;
    config.channel_config = header.channel_config;
    config.channel_count = channelsForConfig(header.channel_config);
    return config;
}

bool hasAdtsSyncword(const uint8_t* data, size_t size) {
    return data && size >= 2 && data[0] == (kAdtsSyncword >> 4) && (data[1] & 0xF0) == 0xF0;
}

}  // namespace player::audio::aac
