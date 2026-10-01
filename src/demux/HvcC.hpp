#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace player::demux {

struct HvccNalUnit {
    uint8_t nal_type = 0;
    std::vector<uint8_t> data;
};

// Parsed HEVCDecoderConfigurationRecord (ISO/IEC 14496-15).
struct HvccInfo {
    bool valid = false;
    uint8_t configuration_version = 0;
    uint8_t profile_space = 0;
    bool tier_flag = false;
    uint8_t profile_idc = 0;
    uint32_t profile_compatibility_flags = 0;
    uint64_t constraint_indicator_flags = 0;  // 48 bits
    uint8_t level_idc = 0;
    uint16_t min_spatial_segmentation_idc = 0;
    uint8_t parallelism_type = 0;
    uint8_t chroma_format_idc = 1;
    uint8_t bit_depth_luma_minus8 = 0;
    uint8_t bit_depth_chroma_minus8 = 0;
    uint16_t avg_frame_rate = 0;
    uint8_t constant_frame_rate = 0;
    uint8_t num_temporal_layers = 1;
    bool temporal_id_nested = true;
    uint8_t length_size = 4;  // 1..4
    std::vector<HvccNalUnit> nals;
};

// Parses an hvcC record. Returns false and fills *error on malformed input.
bool parseHvcc(const uint8_t* data, size_t size, HvccInfo& out, std::string* error = nullptr);

// Human-readable profile/level strings for the stats panel.
std::string hvccProfileName(const HvccInfo& info);
std::string hvccLevelName(const HvccInfo& info);

}  // namespace player::demux
