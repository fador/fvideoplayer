#include "audio/opus/RangeDecoder.hpp"

#include <algorithm>

namespace player::audio::opus {
namespace {

constexpr unsigned kSymBits = 8;
constexpr unsigned kCodeBits = 32;
constexpr uint32_t kCodeTop = 1u << (kCodeBits - 1);
constexpr uint32_t kCodeBot = kCodeTop >> kSymBits;
constexpr unsigned kCodeExtra = (kCodeBits - 2) % kSymBits + 1;  // 7
constexpr uint32_t kSymMax = (1u << kSymBits) - 1;
constexpr unsigned kBitRes = 3;  // ec_tell_frac resolution

unsigned ilog(uint32_t value) {
    unsigned bits = 0;
    while (value != 0) {
        ++bits;
        value >>= 1;
    }
    return bits;
}

}  // namespace

RangeDecoder::RangeDecoder(const uint8_t* data, size_t size) : data_(data), size_(size) {
    bits_total_ = static_cast<int>(kCodeBits + 1 - ((kCodeBits - kCodeExtra) / kSymBits) * kSymBits);
    range_ = 1u << kCodeExtra;
    rem_ = readByte();
    value_ = range_ - 1 - (rem_ >> (kSymBits - kCodeExtra));
    normalize();
}

uint32_t RangeDecoder::readByte() {
    return offset_ < size_ ? data_[offset_++] : 0;
}

uint32_t RangeDecoder::readByteFromEnd() {
    return end_offset_ < size_ ? data_[size_ - ++end_offset_] : 0;
}

void RangeDecoder::normalize() {
    while (range_ <= kCodeBot) {
        bits_total_ += static_cast<int>(kSymBits);
        range_ <<= kSymBits;
        uint32_t sym = rem_;
        rem_ = readByte();
        sym = ((sym << kSymBits) | rem_) >> (kSymBits - kCodeExtra);
        value_ = ((value_ << kSymBits) + (kSymMax & ~sym)) & (kCodeTop - 1);
    }
}

uint32_t RangeDecoder::decode(uint32_t total) {
    scale_ = range_ / total;
    const uint32_t s = value_ / scale_;
    return total - std::min(s + 1, total);
}

uint32_t RangeDecoder::decodeBin(unsigned bits) {
    scale_ = range_ >> bits;
    const uint32_t s = value_ / scale_;
    return (1u << bits) - std::min(s + 1, 1u << bits);
}

void RangeDecoder::update(uint32_t low, uint32_t high, uint32_t total) {
    const uint32_t s = scale_ * (total - high);
    value_ -= s;
    range_ = low > 0 ? scale_ * (high - low) : range_ - s;
    normalize();
}

bool RangeDecoder::decodeBitLogp(unsigned logp) {
    const uint32_t range = range_;
    const uint32_t value = value_;
    const uint32_t split = range >> logp;
    const bool result = value < split;
    if (!result) {
        value_ = value - split;
    }
    range_ = result ? split : range - split;
    normalize();
    return result;
}

int RangeDecoder::decodeIcdf(const uint8_t* icdf, unsigned ftb) {
    uint32_t range = range_;
    const uint32_t value = value_;
    const uint32_t unit = range >> ftb;
    int symbol = -1;
    uint32_t previous = 0;
    do {
        previous = range;
        range = unit * icdf[++symbol];
    } while (value < range);
    value_ = value - range;
    range_ = previous - range;
    normalize();
    return symbol;
}

int RangeDecoder::decodeIcdf16(const uint16_t* icdf, unsigned ftb) {
    uint32_t range = range_;
    const uint32_t value = value_;
    const uint32_t unit = range >> ftb;
    int symbol = -1;
    uint32_t previous = 0;
    do {
        previous = range;
        range = unit * icdf[++symbol];
    } while (value < range);
    value_ = value - range;
    range_ = previous - range;
    normalize();
    return symbol;
}

uint32_t RangeDecoder::decodeUint(uint32_t total) {
    // Uniform integers split into a range-coded high part and raw low bits.
    total -= 1;
    const unsigned bits = ilog(total);
    if (bits > 8) {
        const unsigned extra = bits - 8;
        const uint32_t split = (total >> extra) + 1;
        const uint32_t s = decode(split);
        update(s, s + 1, split);
        const uint32_t value = (s << extra) | decodeBits(extra);
        if (value <= total) {
            return value;
        }
        error_ = true;
        return total;
    }
    const uint32_t s = decode(total + 1);
    update(s, s + 1, total + 1);
    return s;
}

uint32_t RangeDecoder::decodeBits(unsigned bits) {
    uint32_t window = end_window_;
    int available = end_bits_;
    if (static_cast<unsigned>(available) < bits) {
        do {
            window |= readByteFromEnd() << available;
            available += static_cast<int>(kSymBits);
        } while (available <= 32 - static_cast<int>(kSymBits));
    }
    const uint32_t result = window & ((1u << bits) - 1u);
    window >>= bits;
    available -= static_cast<int>(bits);
    end_window_ = window;
    end_bits_ = available;
    bits_total_ += static_cast<int>(bits);
    return result;
}

int RangeDecoder::tell() const {
    return bits_total_ - static_cast<int>(ilog(range_));
}

int RangeDecoder::tellFrac() const {
    // Fractional part from a linear estimate corrected by a lookup (entcode.c).
    static const unsigned correction[8] = {35733, 38967, 42495, 46340,
                                           50535, 55109, 60097, 65535};
    const uint32_t nbits = static_cast<uint32_t>(bits_total_) << kBitRes;
    const unsigned l = ilog(range_);
    const uint32_t r = range_ >> (l - 16);
    unsigned b = (r >> 12) - 8;
    b += r > correction[b] ? 1 : 0;
    return static_cast<int>(nbits - ((l << kBitRes) + b));
}

void RangeDecoder::skipToEnd(int total_bits) {
    bits_total_ += total_bits - tell();
}

}  // namespace player::audio::opus
