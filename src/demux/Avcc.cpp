#include "demux/Avcc.hpp"

namespace player::demux {

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
    return true;
}

void appendAnnexBNal(const AvccNalUnit& nal, std::vector<uint8_t>& out) {
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(1);
    out.insert(out.end(), nal.data.begin(), nal.data.end());
}

}  // namespace player::demux
