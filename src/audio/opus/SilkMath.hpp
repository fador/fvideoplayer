#pragma once

#include <cstdint>

namespace player::audio::silk {

// Fixed-point helpers matching the libopus SILK macros (SigProc_FIX.h and
// macros.h). SILK is integer-only, so the port is bit-exact by construction.

constexpr int32_t kInt16Max = 32767;
constexpr int32_t kInt16Min = -32768;
constexpr int32_t kInt32Max = 0x7FFFFFFF;

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

}  // namespace player::audio::silk
