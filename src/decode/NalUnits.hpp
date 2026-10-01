#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace player::decode {

struct NalView {
    const uint8_t* data = nullptr;
    size_t size = 0;
};

// Splits a length-prefixed (ISO/IEC 14496-15) sample into NAL units.
// length_size is 1..4. Empty NAL units are skipped. Returns false when the
// buffer is malformed.
bool splitLengthPrefixed(const uint8_t* data, size_t size, uint8_t length_size,
                         std::vector<NalView>& out);

}  // namespace player::decode
