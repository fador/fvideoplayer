#include "demux/HvcC.hpp"

#include <cstdio>

namespace player::demux {
namespace {

bool fail(std::string* error, const char* message) {
    if (error) {
        *error = message;
    }
    return false;
}

}  // namespace

bool parseHvcc(const uint8_t* data, size_t size, HvccInfo& out, std::string* error) {
    out = HvccInfo{};
    if (!data || size < 23) {
        return fail(error, "hvcC record too short");
    }
    if (data[0] != 1) {
        return fail(error, "unsupported hvcC configurationVersion");
    }

    size_t pos = 0;
    out.configuration_version = data[pos++];
    out.profile_space = static_cast<uint8_t>(data[pos] >> 6);
    out.tier_flag = (data[pos] & 0x20) != 0;
    out.profile_idc = static_cast<uint8_t>(data[pos] & 0x1F);
    ++pos;
    out.profile_compatibility_flags = (static_cast<uint32_t>(data[pos]) << 24) |
                                      (static_cast<uint32_t>(data[pos + 1]) << 16) |
                                      (static_cast<uint32_t>(data[pos + 2]) << 8) |
                                      static_cast<uint32_t>(data[pos + 3]);
    pos += 4;
    out.constraint_indicator_flags = 0;
    for (int i = 0; i < 6; ++i) {
        out.constraint_indicator_flags = (out.constraint_indicator_flags << 8) | data[pos++];
    }
    out.level_idc = data[pos++];
    out.min_spatial_segmentation_idc = static_cast<uint16_t>(((data[pos] & 0x0F) << 8) | data[pos + 1]);
    pos += 2;
    out.parallelism_type = static_cast<uint8_t>(data[pos] & 0x03);
    ++pos;
    out.chroma_format_idc = static_cast<uint8_t>(data[pos] & 0x03);
    ++pos;
    out.bit_depth_luma_minus8 = static_cast<uint8_t>(data[pos] & 0x07);
    ++pos;
    out.bit_depth_chroma_minus8 = static_cast<uint8_t>(data[pos] & 0x07);
    ++pos;
    out.avg_frame_rate = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
    pos += 2;
    out.constant_frame_rate = static_cast<uint8_t>(data[pos] >> 6);
    out.num_temporal_layers = static_cast<uint8_t>((data[pos] >> 3) & 0x07);
    out.temporal_id_nested = (data[pos] & 0x04) != 0;
    out.length_size = static_cast<uint8_t>((data[pos] & 0x03) + 1);
    ++pos;
    const uint8_t num_arrays = data[pos++];

    for (uint8_t array = 0; array < num_arrays; ++array) {
        if (pos + 3 > size) {
            return fail(error, "hvcC array header truncated");
        }
        const uint8_t nal_type = static_cast<uint8_t>(data[pos] & 0x3F);
        ++pos;  // array_completeness + reserved + nal_unit_type
        const uint16_t num_nalus = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
        pos += 2;

        for (uint16_t i = 0; i < num_nalus; ++i) {
            if (pos + 2 > size) {
                return fail(error, "hvcC NAL length truncated");
            }
            const uint16_t nal_size = static_cast<uint16_t>((data[pos] << 8) | data[pos + 1]);
            pos += 2;
            if (pos + nal_size > size) {
                return fail(error, "hvcC NAL data truncated");
            }
            HvccNalUnit unit;
            unit.nal_type = nal_type;
            unit.data.assign(data + pos, data + pos + nal_size);
            out.nals.push_back(std::move(unit));
            pos += nal_size;
        }
    }

    out.valid = true;
    return true;
}

std::string hvccProfileName(const HvccInfo& info) {
    switch (info.profile_idc) {
        case 1:
            return "Main";
        case 2:
            return "Main 10";
        case 3:
            return "Main Still Picture";
        case 4:
            return "Range Extensions";
        default:
            break;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "Profile %u", info.profile_idc);
    return buffer;
}

std::string hvccLevelName(const HvccInfo& info) {
    const int major = info.level_idc / 30;
    const int minor = (info.level_idc % 30) / 3;
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "L%d.%d", major, minor);
    return buffer;
}

}  // namespace player::demux
