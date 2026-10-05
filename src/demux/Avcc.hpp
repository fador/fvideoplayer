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
};

// Parses an avcC record. Returns false and fills `error` when the record is
// too short or malformed.
bool parseAvcc(const uint8_t* data, size_t size, AvccInfo& out,
               std::string* error = nullptr);

// Appends a NAL unit to an Annex B byte stream, adding the four byte start
// code.
void appendAnnexBNal(const AvccNalUnit& nal, std::vector<uint8_t>& out);

}  // namespace player::demux
