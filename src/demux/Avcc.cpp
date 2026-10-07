#include "demux/Avcc.hpp"

#include <cstdio>

namespace player::demux {
namespace {

// Reads the fields the player needs from a sequence parameter set NAL: the
// profile, level, chroma format and luma bit depth. Emulation prevention
// bytes are removed first. Parsing stops after the bit depth, so the rest of
// the SPS does not have to be handled. Returns false on truncated data.
bool parseSpsHeader(const std::vector<uint8_t>& nal, AvccInfo& out) {
    if (nal.size() < 4) {
        return false;
    }
    std::vector<uint8_t> rbsp;
    rbsp.reserve(nal.size());
    int zeros = 0;
    for (size_t i = 1; i < nal.size(); ++i) {
        const uint8_t byte = nal[i];
        if (zeros >= 2 && byte == 0x03) {
            zeros = 0;
            continue;  // emulation prevention byte
        }
        rbsp.push_back(byte);
        zeros = byte == 0 ? zeros + 1 : 0;
    }

    size_t bit_pos = 0;
    const auto read_bits = [&](int count) -> uint32_t {
        uint32_t value = 0;
        for (int i = 0; i < count; ++i) {
            const size_t index = bit_pos >> 3;
            if (index >= rbsp.size()) {
                bit_pos = rbsp.size() * 8 + 1;  // force the next call to fail
                return value;
            }
            value = (value << 1) | ((rbsp[index] >> (7 - (bit_pos & 7))) & 1);
            ++bit_pos;
        }
        return value;
    };
    const auto read_ue = [&]() -> uint32_t {
        int leading_zeros = 0;
        while (bit_pos < rbsp.size() * 8 &&
               ((rbsp[bit_pos >> 3] >> (7 - (bit_pos & 7))) & 1) == 0) {
            ++leading_zeros;
            ++bit_pos;
        }
        if (bit_pos >= rbsp.size() * 8) {
            bit_pos = rbsp.size() * 8 + 1;
            return 0;
        }
        ++bit_pos;  // the terminating 1
        uint32_t value = 0;
        for (int i = 0; i < leading_zeros; ++i) {
            if (bit_pos >= rbsp.size() * 8) {
                bit_pos = rbsp.size() * 8 + 1;
                return value;
            }
            value = (value << 1) | ((rbsp[bit_pos >> 3] >> (7 - (bit_pos & 7))) & 1);
            ++bit_pos;
        }
        if (leading_zeros >= 32) {
            bit_pos = rbsp.size() * 8 + 1;
            return value;
        }
        return value + ((1u << leading_zeros) - 1u);
    };

    out.profile_idc = static_cast<uint8_t>(read_bits(8));
    read_bits(8);  // constraint set flags and reserved bits
    out.level_idc = static_cast<uint8_t>(read_bits(8));
    read_ue();  // seq_parameter_set_id

    bool high_profile = false;
    switch (out.profile_idc) {
        case 100:
        case 110:
        case 122:
        case 244:
        case 44:
        case 83:
        case 86:
        case 118:
        case 128:
        case 138:
        case 139:
        case 134:
        case 135:
            high_profile = true;
            break;
        default:
            break;
    }
    if (high_profile) {
        out.chroma_format_idc = static_cast<int>(read_ue());
        if (out.chroma_format_idc == 3) {
            read_bits(1);  // separate_colour_plane_flag
        }
        out.bit_depth_luma = 8 + static_cast<int>(read_ue());
        read_ue();  // bit_depth_chroma_minus8
    }
    return bit_pos <= rbsp.size() * 8;
}

}  // namespace

bool parseAvcc(const uint8_t* data, size_t size, AvccInfo& out, std::string* error) {
    out = AvccInfo{};
    if (data == nullptr || size < 7) {
        if (error) {
            *error = "avcC record is too short";
        }
        return false;
    }
    if (data[0] != 1) {
        if (error) {
            *error = "unsupported avcC configurationVersion";
        }
        return false;
    }
    out.length_size = static_cast<uint8_t>((data[4] & 0x03) + 1);
    size_t pos = 5;
    const uint8_t sps_count = data[pos++] & 0x1F;
    for (uint8_t i = 0; i < sps_count; ++i) {
        if (pos + 2 > size) {
            if (error) {
                *error = "truncated avcC SPS table";
            }
            return false;
        }
        const size_t length = static_cast<size_t>((data[pos] << 8) | data[pos + 1]);
        pos += 2;
        if (length == 0 || pos + length > size) {
            if (error) {
                *error = "truncated avcC SPS";
            }
            return false;
        }
        out.sps.push_back({std::vector<uint8_t>(data + pos, data + pos + length)});
        pos += length;
    }
    if (pos >= size) {
        if (error) {
            *error = "avcC record has no PPS table";
        }
        return false;
    }
    const uint8_t pps_count = data[pos++];
    for (uint8_t i = 0; i < pps_count; ++i) {
        if (pos + 2 > size) {
            if (error) {
                *error = "truncated avcC PPS table";
            }
            return false;
        }
        const size_t length = static_cast<size_t>((data[pos] << 8) | data[pos + 1]);
        pos += 2;
        if (length == 0 || pos + length > size) {
            if (error) {
                *error = "truncated avcC PPS";
            }
            return false;
        }
        out.pps.push_back({std::vector<uint8_t>(data + pos, data + pos + length)});
        pos += length;
    }
    if (out.sps.empty()) {
        if (error) {
            *error = "avcC record has no SPS";
        }
        return false;
    }
    // The SPS fields are informational; a decode still works without them.
    parseSpsHeader(out.sps.front().data, out);
    return true;
}

std::string avcProfileName(uint8_t profile_idc) {
    switch (profile_idc) {
        case 66:
            return "Baseline";
        case 77:
            return "Main";
        case 88:
            return "Extended";
        case 100:
            return "High";
        case 110:
            return "High 10";
        case 122:
            return "High 4:2:2";
        case 244:
            return "High 4:4:4 Predictive";
        case 44:
            return "CAVLC 4:4:4 Intra";
        case 83:
            return "Scalable Baseline";
        case 86:
            return "Scalable High";
        case 118:
            return "Multiview High";
        case 128:
            return "Stereo High";
        default:
            break;
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "Profile %u", profile_idc);
    return buffer;
}

std::string avcLevelName(uint8_t level_idc) {
    if (level_idc == 0) {
        return {};
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "L%u.%u", level_idc / 10, level_idc % 10);
    return buffer;
}

void appendAnnexBNal(const AvccNalUnit& nal, std::vector<uint8_t>& out) {
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
    out.insert(out.end(), nal.data.begin(), nal.data.end());
}

}  // namespace player::demux
