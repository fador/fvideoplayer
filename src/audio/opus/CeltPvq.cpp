#include "audio/opus/CeltPvq.hpp"

#include <vector>

namespace player::audio::opus {
namespace {

//    The U(n, k) and V(n, k) recurrences from RFC 6716:
//
//      U(n, k) = U(n-1, k) + U(n, k-1) + U(n-1, k-1)
//      V(n, k) = U(n, k) + U(n, k+1)
//
//    unext() advances a row of any recurrence obeying u[i][j] =
//    u[i-1][j] + u[i][j-1] + u[i-1][j-1]; uprev() reverses it. All arithmetic
//    wraps modulo 2^32, exactly as in the reference decoder.

void unext(uint32_t* u, unsigned length, uint32_t u0) {
    uint32_t u1 = 0;
    unsigned j = 1;
    do {
        u1 = u[j] + u[j - 1] + u0;
        u[j - 1] = u0;
        u0 = u1;
    } while (++j < length);
    u[j - 1] = u0;
}

void uprev(uint32_t* u, unsigned length, uint32_t u0) {
    uint32_t u1 = 0;
    unsigned j = 1;
    do {
        u1 = u[j] - u[j - 1] - u0;
        u[j - 1] = u0;
        u0 = u1;
    } while (++j < length);
    u[j - 1] = u0;
}

// Fills u[0..k+1] with U(n, 0..k+1) and returns V(n, k).
uint32_t ncwrsUrow(unsigned n, unsigned k, uint32_t* u) {
    const unsigned length = k + 2;
    u[0] = 0;
    u[1] = 1;
    unsigned i = 2;
    do {
        u[i] = (i << 1) - 1;
    } while (++i < length);
    for (i = 2; i < n; ++i) {
        unext(u + 1, k + 1, 1);
    }
    return u[k] + u[k + 1];
}

// Index to pulse vector; u holds row n of U and is destroyed.
float cwrsi(int n, int k, uint32_t index, int* out, uint32_t* u) {
    float energy = 0.0f;
    int j = 0;
    do {
        uint32_t p = u[k + 1];
        const int sign = -(index >= p ? 1 : 0);
        index -= p & static_cast<uint32_t>(sign);
        int magnitude = k;
        p = u[k];
        while (p > index) {
            p = u[--k];
        }
        index -= p;
        magnitude -= k;
        const int value = (magnitude + sign) ^ sign;
        out[j] = value;
        energy += static_cast<float>(value) * static_cast<float>(value);
        uprev(u, static_cast<unsigned>(k) + 2, 0);
    } while (++j < n);
    return energy;
}

}  // namespace

uint32_t pvqCount(int n, int k) {
    std::vector<uint32_t> u(static_cast<size_t>(k) + 2);
    return ncwrsUrow(static_cast<unsigned>(n), static_cast<unsigned>(k), u.data());
}

float indexToPulses(int n, int k, uint32_t index, int* out) {
    std::vector<uint32_t> u(static_cast<size_t>(k) + 2);
    ncwrsUrow(static_cast<unsigned>(n), static_cast<unsigned>(k), u.data());
    return cwrsi(n, k, index, out, u.data());
}

float decodePulses(RangeDecoder& decoder, int n, int k, int* out) {
    std::vector<uint32_t> u(static_cast<size_t>(k) + 2);
    const uint32_t total = ncwrsUrow(static_cast<unsigned>(n), static_cast<unsigned>(k), u.data());
    return cwrsi(n, k, decoder.decodeUint(total), out, u.data());
}

}  // namespace player::audio::opus
