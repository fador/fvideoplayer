#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace player::demux {

// One NAL unit of an AVC decoder configuration record, without its length
// prefix (Annex B start codes are added when it is fed to a decoder).
struct AvccNalUnit {
    std::vector<uint8_t> data;
};

// Decoded AVCDecoderConfigurationRecord (ISO/IEC 14496-15, section 5.3.4.1).
struct AvccInfo {
    uint8_t length_size = 4;  // bytes per NAL length prefix in the samples
    std::vector<AvccNalUnit> sps;
    std::vector<AvccNalUnit> pps;
    // Best-effort fields parsed from the first SPS. They keep their defaults
    // when the SPS is missing or truncated.
    uint8_t profile_idc = 0;
    uint8_t level_idc = 0;
    int chroma_format_idc = 1;  // 0 mono, 1 4:2:0, 2 4:2:2, 3 4:4:4
    int bit_depth_luma = 8;
};

// Parses an avcC record. Returns false and fills `error` when the record is
// too short or malformed.
bool parseAvcc(const uint8_t* data, size_t size, AvccInfo& out,
               std::string* error = nullptr);

// Human-readable H.264 profile and level names for the statistics display.
std::string avcProfileName(uint8_t profile_idc);
std::string avcLevelName(uint8_t level_idc);

// Appends a NAL unit to an Annex B byte stream, adding the four byte start
// code.
void appendAnnexBNal(const AvccNalUnit& nal, std::vector<uint8_t>& out);

}  // namespace player::demux
