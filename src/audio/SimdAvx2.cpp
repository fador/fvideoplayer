// AVX2 kernels. This translation unit is compiled with /arch:AVX2 (MSVC) or
// -mavx2 and must only be entered after simd::hasAvx2() returned true.
#include "audio/Simd.hpp"

#include <immintrin.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <vector>

namespace player::audio::simd {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct TwiddleTable {
    std::vector<double> re;
    std::vector<double> im;
    std::vector<size_t> stage_offset;
};

// e^(+i*2*pi*k/len) for every FFT stage, laid out contiguously per stage.
const TwiddleTable& twiddleTable(size_t n) {
    static std::mutex mutex;
    static std::map<size_t, TwiddleTable> cache;
    std::lock_guard<std::mutex> lock(mutex);

    const auto found = cache.find(n);
    if (found != cache.end()) {
        return found->second;
    }

    TwiddleTable table;
    size_t stages = 0;
    for (size_t len = 2; len <= n; len <<= 1) {
        ++stages;
    }
    table.stage_offset.resize(stages + 1);
    size_t total = 0;
    size_t stage = 0;
    for (size_t len = 2; len <= n; len <<= 1, ++stage) {
        table.stage_offset[stage] = total;
        total += len / 2;
    }
    table.stage_offset[stages] = total;
    table.re.resize(total);
    table.im.resize(total);

    stage = 0;
    for (size_t len = 2; len <= n; len <<= 1, ++stage) {
        const double angle = 2.0 * kPi / static_cast<double>(len);
        for (size_t k = 0; k < len / 2; ++k) {
            table.re[table.stage_offset[stage] + k] = std::cos(angle * static_cast<double>(k));
            table.im[table.stage_offset[stage] + k] = std::sin(angle * static_cast<double>(k));
        }
    }
    return cache.emplace(n, std::move(table)).first->second;
}

}  // namespace

void applyGainAvx2(float* samples, size_t count, float gain) {
    const __m256 vgain = _mm256_set1_ps(gain);
    const __m256 vlow = _mm256_set1_ps(-1.0f);
    const __m256 vhigh = _mm256_set1_ps(1.0f);
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        __m256 v = _mm256_loadu_ps(samples + i);
        v = _mm256_mul_ps(v, vgain);
        v = _mm256_max_ps(vlow, _mm256_min_ps(vhigh, v));
        _mm256_storeu_ps(samples + i, v);
    }
    for (; i < count; ++i) {
        samples[i] = std::clamp(samples[i] * gain, -1.0f, 1.0f);
    }
}

void dequantizeBandAvx2(const int16_t* quant, float* spectrum, size_t count, float gain) {
    const float* table = pow34Table();
    const __m256 vgain = _mm256_set1_ps(gain);
    const __m256i vmax = _mm256_set1_epi32(static_cast<int>(kPow34TableSize - 1));
    const __m256i vsign = _mm256_set1_epi32(static_cast<int>(0x80000000u));

    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m128i q16 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(quant + i));
        const __m256i q32 = _mm256_cvtepi16_epi32(q16);
        const __m256i sign = _mm256_and_si256(_mm256_srai_epi32(q32, 31), vsign);
        const __m256i magnitude = _mm256_min_epi32(_mm256_abs_epi32(q32), vmax);
        __m256 values = _mm256_i32gather_ps(table, magnitude, 4);
        values = _mm256_xor_ps(values, _mm256_castsi256_ps(sign));
        values = _mm256_mul_ps(values, vgain);
        _mm256_storeu_ps(spectrum + i, values);
    }
    for (; i < count; ++i) {
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

void windowMultiplyAvx2(float* block, const float* window, size_t count) {
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256 v = _mm256_loadu_ps(block + i);
        const __m256 w = _mm256_loadu_ps(window + i);
        _mm256_storeu_ps(block + i, _mm256_mul_ps(v, w));
    }
    for (; i < count; ++i) {
        block[i] *= window[i];
    }
}

void addWindowedAvx2(float* dst, const float* src, const float* window, size_t count) {
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256 d = _mm256_loadu_ps(dst + i);
        const __m256 s = _mm256_loadu_ps(src + i);
        const __m256 w = _mm256_loadu_ps(window + i);
        _mm256_storeu_ps(dst + i, _mm256_add_ps(d, _mm256_mul_ps(s, w)));
    }
    for (; i < count; ++i) {
        dst[i] += src[i] * window[i];
    }
}

void scaleCopyAvx2(float* dst, const float* src, size_t count, float scale) {
    const __m256 vscale = _mm256_set1_ps(scale);
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256 s = _mm256_loadu_ps(src + i);
        _mm256_storeu_ps(dst + i, _mm256_mul_ps(s, vscale));
    }
    for (; i < count; ++i) {
        dst[i] = src[i] * scale;
    }
}

void midSideAvx2(float* left, float* right, size_t count) {
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256 mid = _mm256_loadu_ps(left + i);
        const __m256 side = _mm256_loadu_ps(right + i);
        _mm256_storeu_ps(left + i, _mm256_add_ps(mid, side));
        _mm256_storeu_ps(right + i, _mm256_sub_ps(mid, side));
    }
    for (; i < count; ++i) {
        const float mid = left[i];
        const float side = right[i];
        left[i] = mid + side;
        right[i] = mid - side;
    }
}

void complexMultiplyAvx2(double* data, const double* twiddles, size_t count) {
    size_t i = 0;
    for (; i + 2 <= count; i += 2) {
        const __m256d x = _mm256_loadu_pd(data + 2 * i);
        const __m256d w = _mm256_loadu_pd(twiddles + 2 * i);
        const __m256d x_re = _mm256_movedup_pd(x);
        const __m256d x_im = _mm256_permute_pd(x, 0xF);
        const __m256d w_re = _mm256_movedup_pd(w);
        const __m256d w_im = _mm256_permute_pd(w, 0xF);
        const __m256d re =
            _mm256_sub_pd(_mm256_mul_pd(x_re, w_re), _mm256_mul_pd(x_im, w_im));
        const __m256d im =
            _mm256_add_pd(_mm256_mul_pd(x_re, w_im), _mm256_mul_pd(x_im, w_re));
        _mm256_storeu_pd(data + 2 * i, _mm256_blend_pd(re, im, 0xA));
    }
    for (; i < count; ++i) {
        const double re = data[2 * i];
        const double im = data[2 * i + 1];
        const double wr = twiddles[2 * i];
        const double wi = twiddles[2 * i + 1];
        data[2 * i] = re * wr - im * wi;
        data[2 * i + 1] = re * wi + im * wr;
    }
}

void dftAvx2(double* data, size_t n) {
    if (n < 2 || (n & (n - 1)) != 0) {
        return;
    }
    const TwiddleTable& table = twiddleTable(n);

    thread_local std::vector<double> re;
    thread_local std::vector<double> im;
    re.resize(n);
    im.resize(n);

    // Split the interleaved complex input so the butterflies stay elementwise.
    for (size_t i = 0; i < n; ++i) {
        re[i] = data[2 * i];
        im[i] = data[2 * i + 1];
    }

    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }

    for (size_t len = 2, stage = 0; len <= n; len <<= 1, ++stage) {
        const size_t half = len >> 1;
        const double* wr = table.re.data() + table.stage_offset[stage];
        const double* wi = table.im.data() + table.stage_offset[stage];
        for (size_t start = 0; start < n; start += len) {
            size_t k = 0;
            for (; k + 4 <= half; k += 4) {
                const __m256d er = _mm256_loadu_pd(&re[start + k]);
                const __m256d ei = _mm256_loadu_pd(&im[start + k]);
                const __m256d odd_re = _mm256_loadu_pd(&re[start + half + k]);
                const __m256d odd_im = _mm256_loadu_pd(&im[start + half + k]);
                const __m256d wrv = _mm256_loadu_pd(wr + k);
                const __m256d wiv = _mm256_loadu_pd(wi + k);
                const __m256d tr =
                    _mm256_sub_pd(_mm256_mul_pd(wrv, odd_re), _mm256_mul_pd(wiv, odd_im));
                const __m256d ti =
                    _mm256_add_pd(_mm256_mul_pd(wrv, odd_im), _mm256_mul_pd(wiv, odd_re));
                _mm256_storeu_pd(&re[start + k], _mm256_add_pd(er, tr));
                _mm256_storeu_pd(&im[start + k], _mm256_add_pd(ei, ti));
                _mm256_storeu_pd(&re[start + half + k], _mm256_sub_pd(er, tr));
                _mm256_storeu_pd(&im[start + half + k], _mm256_sub_pd(ei, ti));
            }
            for (; k < half; ++k) {
                const double odd_re = re[start + half + k];
                const double odd_im = im[start + half + k];
                const double tr = wr[k] * odd_re - wi[k] * odd_im;
                const double ti = wr[k] * odd_im + wi[k] * odd_re;
                const double er = re[start + k];
                const double ei = im[start + k];
                re[start + k] = er + tr;
                im[start + k] = ei + ti;
                re[start + half + k] = er - tr;
                im[start + half + k] = ei - ti;
            }
        }
    }

    for (size_t i = 0; i < n; ++i) {
        data[2 * i] = re[i];
        data[2 * i + 1] = im[i];
    }
}

}  // namespace player::audio::simd
