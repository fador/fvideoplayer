#include "audio/Simd.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
#include <cpuid.h>
#endif

namespace player::audio::simd {
namespace {

#if (defined(_MSC_VER) || defined(__GNUC__)) && \
    (defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__))
#define PLAYER_X86 1
#else
#define PLAYER_X86 0
#endif

#if PLAYER_X86 && defined(_MSC_VER)
bool detectAvx2() {
    int regs[4] = {};
    __cpuid(regs, 0);
    if (regs[0] < 7) {
        return false;
    }
    __cpuid(regs, 1);
    const bool osxsave = (regs[2] & (1 << 27)) != 0;
    const bool avx = (regs[2] & (1 << 28)) != 0;
    const bool fma = (regs[2] & (1 << 12)) != 0;
    if (!osxsave || !avx || !fma) {
        return false;  // /arch:AVX2 code may use FMA
    }
    if ((_xgetbv(0) & 0x6) != 0x6) {  // XMM and YMM state enabled by the OS
        return false;
    }
    __cpuidex(regs, 7, 0);
    return (regs[1] & (1 << 5)) != 0;  // AVX2
}
#elif PLAYER_X86
bool detectAvx2() {
    return __builtin_cpu_supports("avx2") != 0;
}
#else
bool detectAvx2() {
    return false;
}
#endif

std::vector<float> buildPow34Table() {
    std::vector<float> table(kPow34TableSize);
    for (size_t i = 0; i < kPow34TableSize; ++i) {
        table[i] = static_cast<float>(std::pow(static_cast<double>(i), 4.0 / 3.0));
    }
    return table;
}

}  // namespace

bool hasAvx2() {
    static const bool supported = detectAvx2();
    return supported;
}

const float* pow34Table() {
    static const std::vector<float> table = buildPow34Table();
    return table.data();
}

void applyGain(float* samples, size_t count, float gain) {
    if (hasAvx2()) {
        applyGainAvx2(samples, count, gain);
        return;
    }
    applyGainScalar(samples, count, gain);
}

void dequantizeBand(const int16_t* quant, float* spectrum, size_t count, float gain) {
    if (hasAvx2()) {
        dequantizeBandAvx2(quant, spectrum, count, gain);
        return;
    }
    dequantizeBandScalar(quant, spectrum, count, gain);
}

void windowMultiply(float* block, const float* window, size_t count) {
    if (hasAvx2()) {
        windowMultiplyAvx2(block, window, count);
        return;
    }
    windowMultiplyScalar(block, window, count);
}

void addWindowed(float* dst, const float* src, const float* window, size_t count) {
    if (hasAvx2()) {
        addWindowedAvx2(dst, src, window, count);
        return;
    }
    addWindowedScalar(dst, src, window, count);
}

void scaleCopy(float* dst, const float* src, size_t count, float scale) {
    if (hasAvx2()) {
        scaleCopyAvx2(dst, src, count, scale);
        return;
    }
    scaleCopyScalar(dst, src, count, scale);
}

void midSide(float* left, float* right, size_t count) {
    if (hasAvx2()) {
        midSideAvx2(left, right, count);
        return;
    }
    midSideScalar(left, right, count);
}

void complexMultiply(double* data, const double* twiddles, size_t count) {
    if (hasAvx2()) {
        complexMultiplyAvx2(data, twiddles, count);
        return;
    }
    complexMultiplyScalar(data, twiddles, count);
}

void dft(double* interleaved, size_t size) {
    if (hasAvx2()) {
        dftAvx2(interleaved, size);
        return;
    }
    dftScalar(interleaved, size);
}

void applyGainScalar(float* samples, size_t count, float gain) {
    for (size_t i = 0; i < count; ++i) {
        samples[i] = std::clamp(samples[i] * gain, -1.0f, 1.0f);
    }
}

void dequantizeBandScalar(const int16_t* quant, float* spectrum, size_t count, float gain) {
    const float* table = pow34Table();
    for (size_t i = 0; i < count; ++i) {
        const int32_t value = quant[i];
        if (value == 0) {
            spectrum[i] = 0.0f;
            continue;
        }
        const size_t magnitude = static_cast<size_t>(value < 0 ? -value : value);
        const float dequantized = table[std::min(magnitude, kPow34TableSize - 1)] * gain;
        spectrum[i] = value < 0 ? -dequantized : dequantized;
    }
}

void windowMultiplyScalar(float* block, const float* window, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        block[i] *= window[i];
    }
}

void addWindowedScalar(float* dst, const float* src, const float* window, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        dst[i] += src[i] * window[i];
    }
}

void scaleCopyScalar(float* dst, const float* src, size_t count, float scale) {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = src[i] * scale;
    }
}

void midSideScalar(float* left, float* right, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const float mid = left[i];
        const float side = right[i];
        left[i] = mid + side;
        right[i] = mid - side;
    }
}

void complexMultiplyScalar(double* data, const double* twiddles, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const double re = data[2 * i];
        const double im = data[2 * i + 1];
        const double wr = twiddles[2 * i];
        const double wi = twiddles[2 * i + 1];
        data[2 * i] = re * wr - im * wi;
        data[2 * i + 1] = re * wi + im * wr;
    }
}

// Iterative radix-2 DFT with a positive exponent.
void dftScalar(double* data, size_t n) {
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[2 * i], data[2 * j]);
            std::swap(data[2 * i + 1], data[2 * j + 1]);
        }
    }

    constexpr double kPi = 3.14159265358979323846;
    for (size_t length = 2; length <= n; length <<= 1) {
        const double angle = 2.0 * kPi / static_cast<double>(length);
        const double step_re = std::cos(angle);
        const double step_im = std::sin(angle);
        const size_t half = length >> 1;
        for (size_t start = 0; start < n; start += length) {
            double twiddle_re = 1.0;
            double twiddle_im = 0.0;
            for (size_t k = 0; k < half; ++k) {
                const size_t even = 2 * (start + k);
                const size_t odd = 2 * (start + k + half);
                const double or_re = data[odd];
                const double or_im = data[odd + 1];
                const double tr = twiddle_re * or_re - twiddle_im * or_im;
                const double ti = twiddle_re * or_im + twiddle_im * or_re;
                const double er = data[even];
                const double ei = data[even + 1];
                data[even] = er + tr;
                data[even + 1] = ei + ti;
                data[odd] = er - tr;
                data[odd + 1] = ei - ti;

                const double next_re = twiddle_re * step_re - twiddle_im * step_im;
                twiddle_im = twiddle_re * step_im + twiddle_im * step_re;
                twiddle_re = next_re;
            }
        }
    }
}

}  // namespace player::audio::simd
