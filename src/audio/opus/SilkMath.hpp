#pragma once

#include <cstdint>

namespace player::audio::silk {

// Fixed-point helpers matching the libopus SILK macros (SigProc_FIX.h and
// macros.h). SILK is integer-only, so the port is bit-exact by construction.

constexpr int32_t kInt16Max = 32767;
constexpr int32_t kInt16Min = -32768;
constexpr int32_t kInt32Max = 0x7FFFFFFF;
constexpr int32_t kInt32Min = -0x7FFFFFFF - 1;

inline int32_t sat16(int32_t a) {
    return a > kInt16Max ? kInt16Max : (a < kInt16Min ? kInt16Min : a);
}

// (a * (int16)b) >> 16
inline int32_t smulwb(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * static_cast<int16_t>(b)) >> 16);
}

// a + ((b * (int16)c) >> 16)
inline int32_t smlawb(int32_t a, int32_t b, int32_t c) {
    return a + static_cast<int32_t>((static_cast<int64_t>(b) * static_cast<int16_t>(c)) >> 16);
}

// (int16)a * (int16)b
inline int32_t smulbb(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<int16_t>(a)) *
           static_cast<int32_t>(static_cast<int16_t>(b));
}

inline int32_t rshiftRound(int32_t a, int shift) {
    return shift == 1 ? ((a >> 1) + (a & 1)) : (((a >> (shift - 1)) + 1) >> 1);
}

inline int32_t addSat16(int32_t a, int32_t b) {
    return sat16(a + b);
}

// silk_LIMIT: clamps a between limit1 and limit2 in either order.
inline int32_t limit(int32_t a, int32_t limit1, int32_t limit2) {
    const int32_t lo = limit1 < limit2 ? limit1 : limit2;
    const int32_t hi = limit1 < limit2 ? limit2 : limit1;
    return a < lo ? lo : (a > hi ? hi : a);
}

// Insertion sort, increasing (silk_insertion_sort_increasing_all_values_int16).
inline void insertionSortIncreasing(int16_t* a, int n) {
    for (int i = 1; i < n; ++i) {
        const int16_t value = a[i];
        int j = i - 1;
        while (j >= 0 && value < a[j]) {
            a[j + 1] = a[j];
            --j;
        }
        a[j + 1] = value;
    }
}

// Number of leading zeros, 32 for zero (silk_CLZ32).
inline int32_t clz32(int32_t x) {
    uint32_t value = static_cast<uint32_t>(x);
    int bits = 0;
    while (value != 0) {
        ++bits;
        value >>= 1;
    }
    return 32 - bits;
}

// (a * b) >> 32
inline int32_t smmul(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 32);
}

// silk_MUL: 32-bit multiply with wraparound.
inline int32_t mul32(int32_t a, int32_t b) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b));
}

// (a * b) >> 16 with a 32-bit result
inline int32_t smulww(int32_t a, int32_t b) {
    return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 16);
}

// a + ((b * c) >> 16)
inline int32_t smlaww(int32_t a, int32_t b, int32_t c) {
    return static_cast<int32_t>(a + ((static_cast<int64_t>(b) * c) >> 16));
}

inline int64_t smull(int32_t a, int32_t b) {
    return static_cast<int64_t>(a) * b;
}

inline int64_t rshiftRound64(int64_t a, int shift) {
    return shift == 1 ? ((a >> 1) + (a & 1)) : (((a >> (shift - 1)) + 1) >> 1);
}

inline int32_t subSat32(int32_t a, int32_t b) {
    const int64_t result = static_cast<int64_t>(a) - b;
    return result > kInt32Max ? kInt32Max : (result < kInt32Min ? kInt32Min
                                                                : static_cast<int32_t>(result));
}

inline int32_t addSat32(int32_t a, int32_t b) {
    const int64_t result = static_cast<int64_t>(a) + b;
    return result > kInt32Max ? kInt32Max : (result < kInt32Min ? kInt32Min
                                                                : static_cast<int32_t>(result));
}

// a + (int16)b * (int16)c with a wrapping add (silk_SMLABB_ovflw).
inline int32_t smlabbOvflw(int32_t a, int32_t b, int32_t c) {
    const int32_t product = static_cast<int32_t>(static_cast<int16_t>(b)) *
                            static_cast<int32_t>(static_cast<int16_t>(c));
    return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(product));
}

inline int32_t lshiftSat32(int32_t a, int shift) {
    return limit(a, kInt32Min >> shift, kInt32Max >> shift) << shift;
}

// (int16)(a >> 16) * (int16)(b >> 16)
inline int32_t smultt(int32_t a, int32_t b) {
    return (a >> 16) * (b >> 16);
}

// a - (b << shift), wrapping
inline int32_t subLshift32(int32_t a, int32_t b, int shift) {
    return static_cast<int32_t>(static_cast<uint32_t>(a) - (static_cast<uint32_t>(b) << shift));
}

inline int32_t div32(int32_t a, int32_t b) {
    return a / b;
}

inline int32_t div32_16(int32_t a, int32_t b) {
    return a / static_cast<int32_t>(static_cast<int16_t>(b));
}

// 32-bit rotate right (silk_ROR32).
inline int32_t ror32(int32_t a, int rot) {
    const uint32_t x = static_cast<uint32_t>(a);
    if (rot == 0) {
        return a;
    }
    if (rot < 0) {
        const uint32_t m = static_cast<uint32_t>(-rot);
        return static_cast<int32_t>((x << m) | (x >> (32 - m)));
    }
    const uint32_t r = static_cast<uint32_t>(rot);
    return static_cast<int32_t>((x << (32 - r)) | (x >> r));
}

// Leading zeros and the 7 bits right after the leading one (silk_CLZ_FRAC).
inline void clzFrac(int32_t in, int32_t* lz, int32_t* frac_q7) {
    *lz = clz32(in);
    *frac_q7 = ror32(in, 24 - *lz) & 0x7F;
}

// Approximate square root (silk_SQRT_APPROX).
inline int32_t sqrtApprox(int32_t x) {
    if (x <= 0) {
        return 0;
    }
    int32_t lz;
    int32_t frac_q7;
    clzFrac(x, &lz, &frac_q7);

    int32_t y = (lz & 1) ? 32768 : 46214;  // 46214 = sqrt(2) * 32768
    y >>= lz >> 1;
    return smlawb(y, y, smulbb(213, frac_q7));
}

// Linear congruential PRNG (silk_RAND).
inline int32_t rand32(int32_t seed) {
    return static_cast<int32_t>(907633515u + static_cast<uint32_t>(seed) * 196314165u);
}

}  // namespace player::audio::silk
