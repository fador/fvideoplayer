#pragma once

#include <cstddef>
#include <cstdint>

namespace player::audio::opus {

// Opus range decoder (RFC 6716 section 4.1), the entropy coder shared by the
// SILK and CELT layers. Symbols are decoded from cumulative frequency ranges
// read forward; raw bits are read backward from the end of the packet.
class RangeDecoder {
public:
    RangeDecoder(const uint8_t* data, size_t size);

    // Decodes a symbol in [0, total) with uniform probability. The returned
    // value must be passed to update().
    uint32_t decode(uint32_t total);
    uint32_t decodeBin(unsigned bits);
    void update(uint32_t low, uint32_t high, uint32_t total);

    // Decodes a bit whose probability of being one is 1 / (1 << logp).
    bool decodeBitLogp(unsigned logp);
    // Decodes a symbol from an inverse CDF (decreasing values ending at 0).
    int decodeIcdf(const uint8_t* icdf, unsigned ftb);
    int decodeIcdf16(const uint16_t* icdf, unsigned ftb);
    // Uniform integer in [0, total); raw bits from the packet end.
    uint32_t decodeUint(uint32_t total);
    uint32_t decodeBits(unsigned bits);

    // Whole bits consumed so far (ec_tell).
    int tell() const;
    // Bits consumed so far in 1/8 bit resolution (ec_tell_frac).
    int tellFrac() const;
    // Pretends total_bits bits were consumed (used by the CELT silence flag).
    void skipToEnd(int total_bits);
    // Internal range state, used as the CELT folding seed (dec->rng).
    uint32_t rng() const { return range_; }
    size_t size() const { return size_; }
    bool error() const { return error_; }

private:
    uint32_t readByte();
    uint32_t readByteFromEnd();
    void normalize();

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t offset_ = 0;
    size_t end_offset_ = 0;
    uint32_t end_window_ = 0;
    int end_bits_ = 0;
    int bits_total_ = 0;
    uint32_t range_ = 0;
    uint32_t value_ = 0;
    uint32_t scale_ = 0;
    uint32_t rem_ = 0;
    bool error_ = false;
};

}  // namespace player::audio::opus
