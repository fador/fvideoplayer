#include "decode/mpeg12/Mpeg12Vlc.hpp"

namespace player::decode::mpeg12 {

namespace {

// Sign-extends `value` into the MPEG-1 motion vector range
// [-2^(f_code+3), 2^(f_code+3)-1].
int signExtend(int value, unsigned bits) {
    const int limit = 1 << (bits - 1);
    value &= (limit << 1) - 1;
    return value >= limit ? value - (limit << 1) : value;
}

}  // namespace

const VlcEntry* matchVlc(BitReader& reader, const VlcEntry* table, size_t count,
                         unsigned max_bits) {
    for (unsigned bits = max_bits; bits >= 1; --bits) {
        const uint32_t window = reader.peek(bits);
        for (size_t i = 0; i < count; ++i) {
            if (table[i].bits == bits && table[i].code == window) {
                reader.skip(bits);
                return &table[i];
            }
        }
    }
    return nullptr;
}

int readVlc(BitReader& reader, const VlcEntry* table, size_t count, unsigned max_bits) {
    const VlcEntry* entry = matchVlc(reader, table, count, max_bits);
    if (entry == nullptr) {
        reader.setError();
        return -1;
    }
    return entry->value;
}

bool readMbAddressIncrement(BitReader& reader, int& increment, bool& end_of_slice) {
    increment = 0;
    end_of_slice = false;
    for (;;) {
        const int symbol = readVlc(reader, kMbAddrIncr, kMbAddrIncrCount, 11);
        if (symbol < 0) {
            return false;
        }
        if (symbol == kMbAddrIncrEnd) {
            end_of_slice = true;
            return true;
        }
        if (symbol == kMbAddrIncrStuffing) {
            continue;
        }
        if (symbol == kMbAddrIncrEscape) {
            increment += 33;
            continue;
        }
        increment += symbol;
        return true;
    }
}

int readCodedBlockPattern(BitReader& reader) {
    return readVlc(reader, kCodedBlockPattern, kCbpCount, 9);
}

int readMotionVector(BitReader& reader, int predictor, int f_code) {
    const int d = readVlc(reader, kMotionVector, kMotionVectorCount, 10);
    if (d < 0) {
        return predictor;
    }
    if (d == 0) {
        // The one-bit codeword encodes a zero delta, so the vector is unchanged.
        return predictor;
    }
    const bool negative = reader.read(1) != 0;
    const unsigned shift = f_code > 1 ? static_cast<unsigned>(f_code - 1) : 0u;
    int magnitude = d;
    if (shift != 0) {
        magnitude = ((d - 1) << shift) | static_cast<int>(reader.read(shift));
        magnitude += 1;
    }
    if (negative) {
        magnitude = -magnitude;
    }
    // MPEG-1 decodes motion vectors modulo 2^(f_code + 4).
    return signExtend(predictor + magnitude, shift + 5);
}

int readMbTypeP(BitReader& reader) {
    // Not a prefix code, so the longest matching codeword wins.
    for (unsigned bits = 6; bits >= 1; --bits) {
        const uint32_t window = reader.peek(bits);
        for (size_t i = 0; i < kMbTypePCount; ++i) {
            if (kMbTypeP[i].bits == bits && kMbTypeP[i].code == window) {
                reader.skip(bits);
                return static_cast<int>(i);
            }
        }
    }
    reader.setError();
    return -1;
}

int readMbTypeB(BitReader& reader) {
    for (unsigned bits = 6; bits >= 1; --bits) {
        const uint32_t window = reader.peek(bits);
        for (size_t i = 0; i < kMbTypeBCount; ++i) {
            if (kMbTypeB[i].bits == bits && kMbTypeB[i].code == window) {
                reader.skip(bits);
                return static_cast<int>(i);
            }
        }
    }
    reader.setError();
    return -1;
}

int readMbTypeI(BitReader& reader, bool& quantiser_type) {
    quantiser_type = false;
    if (reader.read(1) != 0) {
        return 0;
    }
    if (reader.read(1) != 0) {
        quantiser_type = true;
        return 0;
    }
    return -1;  // reserved `00`
}

int readIntraDcDifference(BitReader& reader, int component) {
    const int size = readVlc(reader, component == 0 ? kDcLuma : kDcChroma, kDcCount, 10);
    if (size < 0) {
        return 0;
    }
    if (size == 0) {
        return 0;
    }
    // The DC differential is not two's complement: a leading one is the
    // value itself, a leading zero means the value minus 2^size - 1. So a
    // size of 7 holding 0001111 is -112, not +15.
    const auto raw = reader.read(static_cast<unsigned>(size));
    if (raw >= (1u << (size - 1))) {
        return static_cast<int>(raw);
    }
    return static_cast<int>(raw) - static_cast<int>((1u << size) - 1u);
}

bool readAcCoefficient(BitReader& reader, AcTable table, int& coefficient_index, int& level) {
    const VlcEntry* entries = table == AcTable::Mpeg2 ? kMpeg2RunLevel : kRunLevel;
    const int symbol = readVlc(reader, entries, kRunLevelCount + 2, 16);
    if (symbol < 0) {
        return false;
    }
    if (symbol == kRunLevelEnd) {
        level = 0;
        return false;
    }

    if (symbol == kRunLevelEscape) {
        // After the escape code both tables carry 6 bits of run and a signed
        // level: 8 bits with an 8-bit widening for MPEG-1, 12 bits for MPEG-2.
        coefficient_index += static_cast<int>(reader.read(6)) + 1;
        if (table == AcTable::Mpeg2) {
            level = reader.readSigned(12);
        } else {
            int value = reader.readSigned(8);
            if (value == -128) {
                value = static_cast<int>(reader.read(8)) - 256;
            } else if (value == 0) {
                value = static_cast<int>(reader.read(8));
            }
            level = value;
        }
        return coefficient_index <= 63;
    }

    coefficient_index += kRunLevelRun[symbol] + 1;
    int magnitude = kRunLevelLevel[symbol];
    if (reader.read(1) != 0) {
        magnitude = -magnitude;
    }
    level = magnitude;
    return coefficient_index <= 63;
}

}  // namespace player::decode::mpeg12



