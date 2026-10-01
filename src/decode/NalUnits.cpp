#include "decode/NalUnits.hpp"

namespace player::decode {

bool splitLengthPrefixed(const uint8_t* data, size_t size, uint8_t length_size,
                         std::vector<NalView>& out) {
    out.clear();
    if (length_size == 0 || length_size > 4) {
        return false;
    }
    size_t pos = 0;
    while (pos < size) {
        if (pos + length_size > size) {
            return false;
        }
        uint32_t nal_size = 0;
        for (uint8_t i = 0; i < length_size; ++i) {
            nal_size = (nal_size << 8) | data[pos + i];
        }
        pos += length_size;
        if (nal_size == 0) {
            continue;
        }
        if (pos + nal_size > size) {
            return false;
        }
        out.push_back({data + pos, nal_size});
        pos += nal_size;
    }
    return true;
}

}  // namespace player::decode
