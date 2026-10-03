#pragma once

#include <cstddef>
#include <cstdint>

namespace player::decode::mpeg12 {

// MSB-first bit reader over an MPEG-1 elementary stream payload.
//
// A read that runs past the end of the buffer raises the error flag and yields
// zero, so a damaged slice can be decoded to its end without a bounds check on
// every field. Callers check error() at slice boundaries.
class BitReader {
public:
    BitReader() = default;
    BitReader(const uint8_t* data, size_t size) { reset(data, size); }

    void reset(const uint8_t* data, size_t size) {
        data_ = data;
        size_ = size;
        bits_consumed_ = 0;
        cache_ = 0;
        cached_ = 0;
        error_ = false;
    }

    // Reads up to 32 bits, most significant bit first. Pending bits are held
    // left aligned, so the next bit is always bit 31 of the cache.
    uint32_t read(unsigned count) {
        if (count == 0) {
            return 0;
        }
        if (count > 32) {
            error_ = true;
            return 0;
        }
        if (cached_ < count) {
            refill();
        }
        if (cached_ < count) {
            error_ = true;
            return 0;
        }
        const uint32_t value = cache_ >> (32u - count);
        cached_ -= count;
        // Shift the bits that were not consumed back to the top of the cache and
        // drop whatever the shift pulled in from below.
        cache_ = count >= 32 ? 0u : ((cache_ << count) & highBits(cached_));
        return value;
    }

    // Reads without advancing. Used for VLC lookahead.
    uint32_t peek(unsigned count) const {
        BitReader copy(*this);
        return copy.read(count);
    }

    // Reads up to 32 bits as a two's-complement signed value.
    int32_t readSigned(unsigned count) {
        if (count == 0) {
            return 0;
        }
        const uint32_t raw = read(count);
        if (count >= 32) {
            return static_cast<int32_t>(raw);
        }
        const uint32_t sign = 1u << (count - 1);
        return static_cast<int32_t>((raw ^ sign) - sign);
    }

    void skip(unsigned count) { (void)read(count); }

    // Bit position of the next unread bit.
    size_t bitPosition() const { return bits_consumed_ - cached_; }
    size_t bitsLeft() const { return size_ * 8 - bitPosition(); }
    bool byteAligned() const { return (bitPosition() & 7) == 0; }

    // Skips to the next byte boundary.
    void alignToByte() {
        const unsigned pad = static_cast<unsigned>((8 - (bitPosition() & 7)) & 7);
        if (pad != 0) {
            skip(pad);
        }
    }

    // skip_1stop_8data_bits: aligns to the next byte boundary and consumes the
    // stuffing byte, which the standard requires to be 0xFF. A short tail is
    // accepted so that the last header of a stream does not raise an error.
    void skipStuffing() {
        alignToByte();
        if (bitsLeft() >= 8) {
            if (read(8) != 0xFF) {
                error_ = true;
            }
        }
    }

    // skip_1stop_8data_bits: skips bits until the reader is byte aligned,
    // requiring every skipped bit to be one. Returns false when a zero was
    // found, which means the header was not terminated as the standard
    // requires. A short tail is accepted.
    bool skipAllOnes() {
        const unsigned pad = static_cast<unsigned>((8 - (bitPosition() & 7)) & 7);
        if (pad == 0) {
            return true;
        }
        const uint32_t skipped = read(pad);
        return skipped == (pad == 32 ? 0xFFFFFFFFu : ((1u << pad) - 1));
    }

    // Number of whole bytes that can be read without touching the tail.
    size_t bytePosition() const { return bitPosition() >> 3; }

    bool error() const { return error_; }
    void clearError() { error_ = false; }
    void setError() { error_ = true; }

private:
    // Mask of the top `bits` bits of the cache.
    static uint32_t highBits(unsigned bits) {
        return bits >= 32 ? 0xFFFFFFFFu : (0xFFFFFFFFu << (32u - bits));
    }

    // Mask of the low `bits` bits.
    static uint32_t lowBits(unsigned bits) {
        return bits >= 32 ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    }

    void refill() {
        while (cached_ <= 24) {
            const size_t byte = bits_consumed_ >> 3;
            if (byte >= size_) {
                break;
            }
            // Place the byte directly below the bits already pending.
            cache_ |= static_cast<uint32_t>(data_[byte]) << (24u - cached_);
            cached_ += 8;
            bits_consumed_ += 8;
        }
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t bits_consumed_ = 0;  // bits pulled out of the buffer, incl. cache
    uint32_t cache_ = 0;        // pending bits, left aligned
    unsigned cached_ = 0;
    bool error_ = false;
};

}  // namespace player::decode::mpeg12



