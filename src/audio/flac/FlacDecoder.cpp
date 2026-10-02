#include "audio/flac/FlacDecoder.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

namespace player::audio::flac {
namespace {

// Bit reader over one frame; big-endian, MSB first (native FLAC streams).
class BitReader {
public:
    BitReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    uint32_t readUint(unsigned bits) {
        uint32_t value = 0;
        for (unsigned i = 0; i < bits; ++i) {
            value = (value << 1) | readBit();
        }
        return value;
    }

    int32_t readInt(unsigned bits) {
        if (bits == 0) {
            return 0;
        }
        const uint32_t value = readUint(bits);
        const uint32_t sign = 1u << (bits - 1);
        return static_cast<int32_t>((value ^ sign) - sign);
    }

    // Counts the zero bits up to and including the next set bit.
    unsigned readUnary() {
        unsigned count = 0;
        while (!overrun_ && readBit() == 0) {
            ++count;
        }
        return count;
    }

    void skipBits(uint64_t count) {
        pos_ += static_cast<size_t>(count);
        if (pos_ > size_ * 8) {
            overrun_ = true;
        }
    }
    unsigned skipUnary() { return readUnary(); }

    size_t bytesConsumed() const { return (pos_ + 7) / 8; }
    bool overrun() const { return overrun_; }

private:
    uint32_t readBit() {
        if (pos_ >= size_ * 8) {
            overrun_ = true;
            return 0;
        }
        const uint32_t bit = (data_[pos_ >> 3] >> (7 - (pos_ & 7))) & 1;
        ++pos_;
        return bit;
    }

    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
    bool overrun_ = false;
};

constexpr uint32_t kFrameSync = 0x3FFE;
constexpr unsigned kFrameSyncBits = 14;
constexpr uint32_t kFrameFooterBytes = 2;
constexpr uint32_t kMaxChannels = 8;
constexpr uint32_t kMaxLpcOrder = 32;
constexpr uint32_t kMaxBlockSize = 65535;

enum class Subframe { Constant, Verbatim, Fixed, Lpc };

struct SubframeHeader {
    Subframe type = Subframe::Constant;
    uint32_t order = 0;
    unsigned wasted_bits = 0;
};

struct FrameHeader {
    uint32_t block_size = 0;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t bits = 0;
    uint32_t channel_assignment = 0;
};

struct DecodedFrame {
    std::array<std::vector<int32_t>, kMaxChannels> samples;
    std::array<unsigned, kMaxChannels> wasted = {};
};

uint32_t channelCount(uint32_t assignment) { return assignment < 8 ? assignment + 1 : 2; }

// Moves a decoded sample back into the 32-bit PCM domain. A subframe that wastes
// all of its bits holds zeros only, and the combined shift then reaches 32 bits.
int32_t toPcm(int32_t sample, uint32_t shift) {
    return shift >= 32 ? 0 : static_cast<int32_t>(static_cast<uint32_t>(sample) << shift);
}

// Frame footer CRC-16, polynomial x^16 + x^15 + x^2 + 1, initialised with 0.
// It covers the whole frame including the sync code, the padding and the
// subframes (RFC 9639 section 9.3).
uint16_t frameFooterCrc(const uint8_t* data, size_t size) {
    uint16_t crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) != 0 ? static_cast<uint16_t>((crc << 1) ^ 0x8005)
                                       : static_cast<uint16_t>(crc << 1);
        }
    }
    return crc;
}

// Reads the frame header including the CRC-8 byte. stream_rate and stream_bits
// come from STREAMINFO and cover the header codes that mean "as in STREAMINFO".
bool readFrameHeader(BitReader& bits, FrameHeader& header, uint32_t stream_rate,
                     uint32_t stream_bits, uint32_t stream_block_size) {
    const uint32_t sync = bits.readUint(kFrameSyncBits);
    const uint32_t reserved = bits.readUint(1);
    const uint32_t blocking = bits.readUint(1);
    if (sync != kFrameSync || reserved != 0 || blocking != 0) {
        return false;
    }
    const uint32_t block_code = bits.readUint(4);
    const uint32_t rate_code = bits.readUint(4);
    header.channel_assignment = bits.readUint(4);
    const uint32_t size_code = bits.readUint(3);
    if (bits.readUint(1) != 0) {
        return false;  // reserved
    }

    header.channels = channelCount(header.channel_assignment);
    switch (size_code) {
        case 0: header.bits = stream_bits; break;
        case 1: header.bits = 8; break;
        case 2: header.bits = 12; break;
        case 4: header.bits = 16; break;
        case 5: header.bits = 20; break;
        case 6: header.bits = 24; break;
        case 7: header.bits = 32; break;
        default: return false;  // reserved
    }

    // The coded frame or sample number is a variable-length UTF-8-like value
    // whose length follows from the leading ones of the first byte. Looking for
    // the first byte without the high bit set walks straight into the CRC byte
    // as soon as the number needs two bytes, which starts at frame 128.
    const uint32_t first = bits.readUint(8);
    unsigned value_bytes = 1;
    if ((first & 0x80) != 0) {
        value_bytes = 0;
        for (uint32_t mask = 0x80; (first & mask) != 0; mask >>= 1) {
            ++value_bytes;
        }
        if (value_bytes < 2 || value_bytes > 7) {
            return false;  // 10xxxxxx is a continuation byte, 11111111 is invalid
        }
    }
    for (unsigned i = 1; i < value_bytes; ++i) {
        if ((bits.readUint(8) & 0xC0) != 0x80) {
            return false;  // every following byte must be a continuation byte
        }
    }

    switch (block_code) {
        case 0: header.block_size = stream_block_size; break;
        case 1: header.block_size = 192; break;
        case 2: header.block_size = 576; break;
        case 3: header.block_size = 1152; break;
        case 4: header.block_size = 2304; break;
        case 5: header.block_size = 4608; break;
case 6: header.block_size = bits.readUint(8) + 1; break;
        case 7: header.block_size = bits.readUint(16) + 1; break;
default:
            // Codes 8 to 15 hold a power of two: 256 up to 32768 (RFC 9639).
            header.block_size = 1u << block_code;
            break;
    }

    static constexpr uint32_t kRates[12] = {0,  88200, 176400, 192000, 8000,  16000,
                                            22050, 24000,  32000,  44100, 48000, 96000};
    if (rate_code == 0) {
        header.sample_rate = stream_rate;
    } else if (rate_code <= 11) {
        header.sample_rate = kRates[rate_code];
    } else if (rate_code == 12) {
        header.sample_rate = bits.readUint(8) * 1000;
    } else if (rate_code == 14) {
        header.sample_rate = bits.readUint(16);
    } else {
        header.sample_rate = bits.readUint(16) * 10;
    }
    bits.readUint(8);  // CRC-8 of the header

    if (bits.overrun()) {
        return false;
    }
if (header.block_size == 0 || header.block_size > kMaxBlockSize ||
        header.channels > kMaxChannels || header.bits < 4 || header.bits > 32) {
        return false;
    }
    return true;
}

bool readSubframeHeader(BitReader& bits, SubframeHeader& subframe) {
    const uint32_t header = bits.readUint(8);
    if ((header & 0x80) != 0) {
        return false;
    }
    const uint32_t type = (header & 0x7E) >> 1;
    if (type == 0) {
        subframe.type = Subframe::Constant;
        subframe.order = 0;
    } else if (type == 1) {
        subframe.type = Subframe::Verbatim;
        subframe.order = 0;
    } else if ((type & 0x20) != 0) {
        subframe.type = Subframe::Lpc;
        subframe.order = (type & 0x1F) + 1;
    } else if ((type & 0x08) != 0) {
        subframe.type = Subframe::Fixed;
        subframe.order = type & 0x07;
        if (subframe.order > 4) {
            return false;
        }
    } else {
        return false;
    }
    subframe.wasted_bits = (header & 1) != 0 ? bits.readUnary() + 1 : 0;
    return !bits.overrun();
}

// The side channel of a decorrelated pair carries one extra bit per sample.
uint32_t subframeBits(const FrameHeader& header, uint32_t channel) {
    uint32_t bits = header.bits;
    if ((header.channel_assignment == 8 || header.channel_assignment == 10) && channel == 1) {
        ++bits;
    } else if (header.channel_assignment == 9 && channel == 0) {
        ++bits;
    }
    return bits;
}

bool use64BitPrediction(uint32_t bits, uint32_t order, uint32_t precision) {
    uint32_t log2_order = 0;
    while ((1u << log2_order) < order) {
        ++log2_order;
    }
    return bits + precision + log2_order > 32;
}

// Sum of the predictor taps. The 64-bit variant is exact, the 32-bit one only
// when the stream is shallow enough for the sum to fit, as the format requires.
template <typename Sum>
Sum sumPrediction(uint32_t order, const int32_t* coefficients, const int32_t* samples) {
    Sum prediction = 0;
    for (uint32_t i = 0; i < order; ++i) {
        prediction += static_cast<Sum>(coefficients[i]) *
                      static_cast<Sum>(samples[-1 - static_cast<ptrdiff_t>(i)]);
    }
    return prediction;
}

int32_t calculatePrediction(uint32_t order, int32_t shift, const int32_t* coefficients,
                            const int32_t* samples, uint32_t bits, uint32_t precision) {
    if (use64BitPrediction(bits, order, precision)) {
        return static_cast<int32_t>(sumPrediction<int64_t>(order, coefficients, samples) >> shift);
    }
    // Both shifts must be arithmetic so that a negative sum rounds towards
    // negative infinity, as the format requires.
    return sumPrediction<int32_t>(order, coefficients, samples) >> shift;
}

int32_t decodeRiceValue(BitReader& bits, unsigned rice_param) {
    const uint32_t quotient = bits.readUnary();
    uint32_t value = quotient << rice_param;
    if (rice_param > 0) {
        value |= bits.readUint(rice_param);
    }
    return (value & 1) ? ~(value >> 1) : static_cast<int32_t>(value >> 1);
}

struct ResidualParams {
    uint32_t order = 0;
    int32_t shift = 0;
    uint32_t precision = 0;
    const int32_t* coefficients = nullptr;
};

// Reads the residual coding header and walks the partitions. When out is null
// the residuals are skipped instead of reconstructed.
bool decodeResidual(BitReader& bits, uint32_t bits_per_sample, uint32_t block_size,
                    const ResidualParams& params, int32_t* out) {
    const uint32_t method = bits.readUint(2);
    if (method > 1) {
        return false;
    }
const uint32_t partition_order = bits.readUint(4);
    if (partition_order > 8 || (block_size >> partition_order) < params.order) {
                        return false;
    }

    int32_t* samples = out ? out + params.order : nullptr;
    uint32_t samples_in_partition = (block_size >> partition_order) - params.order;
    uint32_t partitions_remaining = 1u << partition_order;
    for (;;) {
        uint32_t rice_param = bits.readUint(method == 0 ? 4 : 5);
        const uint32_t escape = method == 0 ? 15u : 31u;
        if (rice_param == escape) {
            const uint32_t unencoded = bits.readUint(5);
            if (out) {
                for (uint32_t i = 0; i < samples_in_partition; ++i) {
                    int32_t residual = unencoded > 0 ? bits.readInt(unencoded) : 0;
                    residual += calculatePrediction(params.order, params.shift,
                                                    params.coefficients, samples + i,
                                                    bits_per_sample, params.precision);
                    samples[i] = residual;
                }
            } else {
                bits.skipBits(static_cast<uint64_t>(unencoded) * samples_in_partition);
            }
        } else {
            for (uint32_t i = 0; i < samples_in_partition; ++i) {
                if (out) {
                    const int32_t residual = decodeRiceValue(bits, rice_param);
                    samples[i] =
                        residual + calculatePrediction(params.order, params.shift,
                                                       params.coefficients, samples + i,
                                                       bits_per_sample, params.precision);
                } else {
                    bits.skipUnary();
                    bits.skipBits(rice_param);
                }
            }
        }

        if (out) {
            samples += samples_in_partition;
        }
        if (partitions_remaining == 1) {
            break;
        }
        --partitions_remaining;
        if (partition_order != 0) {
            samples_in_partition = block_size >> partition_order;
        }
    }
    return !bits.overrun();
}

// Decodes (out != null) or skips (out == null) all subframes of one frame.
bool decodeFrameBody(BitReader& bits, const FrameHeader& header, DecodedFrame* out) {
    static constexpr int32_t kFixedCoefficients[5][4] = {
        {0, 0, 0, 0}, {1, 0, 0, 0}, {2, -1, 0, 0}, {3, -3, 1, 0}, {4, -6, 4, -1}};

for (uint32_t c = 0; c < header.channels; ++c) {
        SubframeHeader subframe;
        if (!readSubframeHeader(bits, subframe)) {
            return false;
        }
        uint32_t bits_per_sample = subframeBits(header, c);
        // Wasting every bit of a subframe is legal: it then codes silence.
        if (bits_per_sample > 32 || subframe.wasted_bits > bits_per_sample) {
            return false;
        }
        bits_per_sample -= subframe.wasted_bits;
        if (subframe.order > header.block_size) {
            return false;
        }

        int32_t* samples = nullptr;
        if (out) {
            out->samples[c].assign(header.block_size, 0);
            samples = out->samples[c].data();
            out->wasted[c] = subframe.wasted_bits;
        }

        ResidualParams params;
        params.order = subframe.order;
        switch (subframe.type) {
            case Subframe::Constant:
                if (out) {
                    const int32_t value = bits.readInt(bits_per_sample);
                    std::fill(samples, samples + header.block_size, value);
                } else {
                    bits.skipBits(bits_per_sample);
                }
                break;
            case Subframe::Verbatim:
                if (out) {
                    for (uint32_t i = 0; i < header.block_size; ++i) {
                        samples[i] = bits.readInt(bits_per_sample);
                    }
                } else {
                    bits.skipBits(static_cast<uint64_t>(header.block_size) * bits_per_sample);
                }
                break;
            case Subframe::Fixed:
                if (out) {
                    for (uint32_t i = 0; i < subframe.order; ++i) {
                        samples[i] = bits.readInt(bits_per_sample);
                    }
                } else {
                    bits.skipBits(static_cast<uint64_t>(subframe.order) * bits_per_sample);
                }
                params.shift = 0;
                params.precision = 4;
                params.coefficients = kFixedCoefficients[subframe.order];
                if (!decodeResidual(bits, bits_per_sample, header.block_size, params, samples)) {
                    return false;
                }
                break;
            case Subframe::Lpc: {
                // Warm-up samples come first, then precision, shift and taps.
                if (out) {
                    for (uint32_t i = 0; i < subframe.order; ++i) {
                        samples[i] = bits.readInt(bits_per_sample);
                    }
                } else {
                    bits.skipBits(static_cast<uint64_t>(subframe.order) * bits_per_sample);
                }
                uint32_t precision = bits.readUint(4);
                if (precision == 15) {
                    return false;
                }
                ++precision;
                const int32_t shift = bits.readInt(5);
                if (shift < 0) {
                    return false;
                }
                std::array<int32_t, kMaxLpcOrder> coefficients = {};
                for (uint32_t i = 0; i < subframe.order; ++i) {
                    coefficients[i] = bits.readInt(precision);
                }
params.shift = shift;
                params.precision = precision;
                params.coefficients = coefficients.data();
                for (uint32_t i = 0; i < subframe.order && i < 8; ++i) {
                }
                if (!decodeResidual(bits, bits_per_sample, header.block_size, params, samples)) {
                    return false;
                }
                break;
            }
        }

// Wasted bits are never stored: the decoder adds them back when shifting
        // the samples into the 32-bit PCM domain (RFC 9639 section 9.2.2).
    }
    return !bits.overrun();
}

// Undoes the channel decorrelation and the wasted bits into interleaved samples.
std::vector<int32_t> interleave(const FrameHeader& header, const DecodedFrame& frame) {
    const uint32_t unused_bits = 32 - header.bits;
    std::vector<int32_t> out(static_cast<size_t>(header.block_size) * header.channels);
    const int32_t* a = frame.samples[0].data();
    const int32_t* b = frame.samples[1].data();
    const uint32_t shift0 = unused_bits + frame.wasted[0];
    const uint32_t shift1 = unused_bits + frame.wasted[1];

    if (header.channel_assignment < 8) {
        for (uint32_t i = 0; i < header.block_size; ++i) {
            for (uint32_t c = 0; c < header.channels; ++c) {
                out[i * header.channels + c] =
                    toPcm(frame.samples[c][i], c == 0 ? shift0 : shift1);
            }
        }
        return out;
    }

    for (uint32_t i = 0; i < header.block_size; ++i) {
        int32_t left = 0;
        int32_t right = 0;
        switch (header.channel_assignment) {
            case 8:  // left / side
                left = toPcm(a[i], shift0);
                right = static_cast<int32_t>(static_cast<uint32_t>(left) - toPcm(b[i], shift1));
                break;
            case 9:  // right / side
                right = toPcm(b[i], shift1);
                left = static_cast<int32_t>(static_cast<uint32_t>(right) + toPcm(a[i], shift0));
                break;
            default: {  // mid / side
                // Rebuild the mid sample with the bit the shift dropped, then
                // halve the sum with the side channel: mid' + side is twice the
                // left sample, so the halving is part of the reconstruction and
                // not of the move into the 32-bit domain. The intermediate is
                // 33 bits wide for a 32-bit stream.
                const int64_t mid = static_cast<int64_t>(a[i]) << frame.wasted[0];
                const int64_t side = static_cast<int64_t>(b[i]) << frame.wasted[1];
                const int64_t mid_side = (mid << 1) | (side & 1);
                left = toPcm(static_cast<int32_t>((mid_side + side) >> 1), unused_bits);
                right = toPcm(static_cast<int32_t>((mid_side - side) >> 1), unused_bits);
                break;
            }
        }
        out[i * 2] = left;
        out[i * 2 + 1] = right;
    }
    return out;
}

}  // namespace

bool Decoder::configure(const uint8_t* config, size_t size, std::string& error) {
    reset();
    if (!config || size == 0) {
        configured_ = true;
        return true;
    }
    // Parse "fLaC" plus the first metadata block (STREAMINFO).
    if (size < 8 + 34 || std::memcmp(config, "fLaC", 4) != 0) {
        error = "not a FLAC metadata header";
        return false;
    }
    if ((config[4] & 0x7F) != 0) {
        error = "FLAC STREAMINFO must be the first metadata block";
        return false;
    }
    const uint8_t* info = config + 8;
    uint64_t packed = 0;
    for (int i = 0; i < 8; ++i) {
        packed = (packed << 8) | info[10 + i];
    }
    sample_rate_ = static_cast<uint32_t>(packed >> 44);
    channels_ = static_cast<uint32_t>(((packed >> 41) & 7) + 1);
    bits_ = static_cast<uint32_t>(((packed >> 36) & 0x1F) + 1);
    // Min/max block size in PCM frames (big endian).
    block_size_ = static_cast<uint32_t>((info[2] << 8) | info[3]);
    if (sample_rate_ == 0 || channels_ == 0 || channels_ > kMaxChannels || bits_ < 4 ||
        bits_ > 32) {
        error = "invalid FLAC STREAMINFO";
        return false;
    }
    configured_ = true;
    return true;
}

void Decoder::flush() {
    sample_rate_ = 0;
    channels_ = 0;
    bits_ = 0;
    block_size_ = 0;
    pts_ = 0;
}

void Decoder::reset() {
    flush();
    configured_ = false;
}

size_t Decoder::skipFrame(const uint8_t* data, size_t size, uint32_t* block_size,
                          uint32_t* sample_rate, uint32_t* channels, uint32_t* bits_per_sample) {
    if (!data || size < 5) {
        return 0;
    }
BitReader bits(data, size);
    FrameHeader header;
    if (!readFrameHeader(bits, header, sample_rate_, bits_, block_size_)) {
        return 0;
    }
    if (!decodeFrameBody(bits, header, nullptr)) {
        return 0;
    }
    if (block_size) {
        *block_size = header.block_size;
    }
    if (sample_rate) {
        *sample_rate = header.sample_rate;
    }
    if (channels) {
        *channels = header.channels;
    }
    if (bits_per_sample) {
        *bits_per_sample = header.bits;
    }
const size_t body_bytes = bits.bytesConsumed();
    if (body_bytes + kFrameFooterBytes > size) {
        return 0;
    }
    // The footer CRC-16 covers the frame up to this point, padding included.
    const uint16_t stored = static_cast<uint16_t>((data[body_bytes] << 8) | data[body_bytes + 1]);
    if (frameFooterCrc(data, body_bytes) != stored) {
        return 0;  // the frame footer CRC does not validate
    }
    return body_bytes + kFrameFooterBytes;
}

Status Decoder::decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error) {
    pcm.clear();
    if (!configured_) {
        error = "FLAC decoder not configured";
        return Status::Invalid;
    }
    if (!data || size < 5) {
        error = "short FLAC frame";
        return Status::Invalid;
    }

    BitReader bits(data, size);
    FrameHeader header;
    if (!readFrameHeader(bits, header, sample_rate_, bits_, block_size_)) {
        error = "invalid FLAC frame header";
        return Status::Invalid;
    }
    if (header.channels > kMaxChannels) {
        error = "unsupported FLAC channel count";
        return Status::Unsupported;
    }

    DecodedFrame frame;
    if (!decodeFrameBody(bits, header, &frame)) {
        error = "corrupt FLAC subframe";
        return Status::Invalid;
    }
    const std::vector<int32_t> interleaved = interleave(header, frame);

sample_rate_ = header.sample_rate;
    channels_ = header.channels;
    bits_ = header.bits;
    // interleave() already moved every sample into the 32-bit PCM domain, using
    // the shift that undoes both the declared bit depth and the wasted bits. A
    // full scale sample therefore sits at 2^31, whatever the file declares, and
    // the conversion to float is the same for every stream.
    constexpr float kPcmScale = 1.0f / 2147483648.0f;  // 2^-31
    pcm.channels = header.channels;
    pcm.sample_rate = header.sample_rate;
    pcm.pts = pts_;
    pcm.samples.resize(interleaved.size());
    for (size_t i = 0; i < interleaved.size(); ++i) {
        pcm.samples[i] = static_cast<float>(interleaved[i]) * kPcmScale;
    }
    pts_ += header.block_size;
    return Status::Ok;
}

}  // namespace player::audio::flac

