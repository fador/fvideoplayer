#pragma once

#include <cstddef>
#include <cstdint>

namespace player::audio {

// MSB-first bit reader over a byte buffer, as used by the AAC syntax. Reads
// happen from the most significant bit of each byte onwards. A failed read
// (past the end) leaves the position unchanged.
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    // Reads up to 32 bits.
    bool read(unsigned count, uint32_t& value) {
        if (count > kMaxReadBits || count > bitsLeft()) {
            return false;
        }
        uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i) {
            const size_t bit = bit_ + i;
            result = (result << 1) | ((data_[bit >> 3] >> (7 - (bit & 7))) & 1u);
        }
        bit_ += count;
        value = result;
        return true;
    }

    // Reads without advancing; used for lookahead (e.g. Huffman prefixes).
    bool peek(unsigned count, uint32_t& value) const {
        if (count > kMaxReadBits || count > bitsLeft()) {
            return false;
        }
        uint32_t result = 0;
        for (unsigned i = 0; i < count; ++i) {
            const size_t bit = bit_ + i;
            result = (result << 1) | ((data_[bit >> 3] >> (7 - (bit & 7))) & 1u);
        }
        value = result;
        return true;
    }

    bool skip(size_t count) {
        if (count > bitsLeft()) {
            return false;
        }
        bit_ += count;
        return true;
    }

    // Skips to the next byte boundary.
    bool alignToByte() {
        return skip((8 - (bit_ & 7)) & 7);
    }

    size_t bitsLeft() const { return size_ * 8 - bit_; }
    size_t bitPosition() const { return bit_; }
    bool byteAligned() const { return (bit_ & 7) == 0; }

private:
    static constexpr unsigned kMaxReadBits = 32;

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t bit_ = 0;
};

}  // namespace player::audio
