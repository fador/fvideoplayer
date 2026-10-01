#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace player::audio::aac {

// Audio object types from ISO/IEC 14496-3. Only a subset is named here.
enum class ObjectType : uint8_t {
    Main = 1,
    Lc = 2,
    Ssr = 3,
    Ltp = 4,
    Sbr = 5,
    Scalable = 6,
    ErLc = 17,
    Ps = 29,
};

// Sampling frequency table from ISO/IEC 14496-3 (index 15 means explicit).
constexpr uint32_t kExplicitSampleRateIndex = 15;
constexpr uint32_t kInvalidSampleRate = 0;

// Decoder configuration parsed from an AudioSpecificConfig, the extradata
// stored by MP4 (esds) and Matroska (CodecPrivate) for AAC tracks.
struct AudioSpecificConfig {
    uint8_t object_type = 0;
    uint32_t sample_rate = 0;
    uint8_t sample_rate_index = 0;  // 0..12 into the ISO sample rate table
    uint8_t channel_config = 0;
    uint32_t channel_count = 0;
    uint32_t extension_sample_rate = 0;
    bool frame_length_short = false;  // 960-sample frames, not supported
    bool sbr_present = false;
    bool ps_present = false;
};

// ADTS frame header, used by raw/AVI AAC streams.
struct AdtsHeader {
    uint8_t object_type = 0;
    uint32_t sample_rate = 0;
    uint8_t sample_rate_index = 0;
    uint8_t channel_config = 0;
    uint16_t frame_length = 0;  // whole frame: header + raw data (+ CRC)
    uint8_t header_length = 0;
    uint8_t raw_blocks = 0;  // number_of_raw_data_blocks_in_frame
};

uint32_t sampleRateForIndex(uint8_t index);
uint8_t nearestSampleRateIndex(uint32_t sample_rate);
uint32_t channelsForConfig(uint8_t channel_config);
const char* objectTypeName(uint8_t object_type);

bool parseAudioSpecificConfig(const uint8_t* data, size_t size, AudioSpecificConfig& config,
                              std::string* error);
bool parseAdtsHeader(const uint8_t* data, size_t size, AdtsHeader& header, std::string* error);

// Configuration implied by an ADTS header (used when no extradata exists).
AudioSpecificConfig configFromAdts(const AdtsHeader& header);

// True when the buffer starts with an ADTS syncword.
bool hasAdtsSyncword(const uint8_t* data, size_t size);

}  // namespace player::audio::aac
