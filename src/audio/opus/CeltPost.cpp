#include "audio/opus/CeltPost.hpp"

#include "audio/opus/CeltVq.hpp"
#include "audio/opus/OpusTables.hpp"

#include <algorithm>
#include <cmath>

namespace player::audio::opus {
namespace {

// Energies above this are clamped to avoid overflow (bands.c).
constexpr float kMaxLogEnergy = 32.0f;

float exp2(float x) {
    return static_cast<float>(std::exp(0.6931471805599453094 * x));
}

uint32_t celtLcgRand(uint32_t seed) {
    return 1664525u * seed + 1013904223u;
}

}  // namespace

void denormaliseBands(const float* x, const float* band_log_e, int start, int end, int m,
                      int downsample, int silence, float* freq) {
    const int frame = m * kCeltShortMdct;
    int bound = m * kCeltEBands[end];
    if (downsample != 1) {
        bound = std::min(bound, frame / downsample);
    }
    if (silence) {
        bound = 0;
        start = 0;
        end = 0;
    }

    float* f = freq;
    x += m * kCeltEBands[start];
    for (int i = 0; i < m * kCeltEBands[start]; ++i) {
        *f++ = 0;
    }
    for (int i = start; i < end; ++i) {
        const int band_end = m * kCeltEBands[i + 1];
        const float lg = band_log_e[i] + kCeltEMeans[i];
        const float g = exp2(std::min(kMaxLogEnergy, lg));
        for (int j = m * kCeltEBands[i]; j < band_end; ++j) {
            *f++ = *x++ * g;
        }
    }
    std::fill(freq + bound, freq + frame, 0.0f);
}

void antiCollapse(float* x, const uint8_t* collapse_masks, int lm, int channels, int size,
                  int start, int end, const float* log_e, const float* prev1_log_e,
                  const float* prev2_log_e, const int* pulses, uint32_t& seed) {
    const int m = 1 << lm;
    for (int i = start; i < end; ++i) {
        const int n0 = kCeltEBands[i + 1] - kCeltEBands[i];

        // Depth in 1/8 bits: how much noise the band deserves.
        const int depth = (1 + pulses[i]) / n0 >> lm;
        const float thresh = 0.5f * exp2(-0.125f * depth);
        const float sqrt_1 = 1.0f / static_cast<float>(std::sqrt(static_cast<float>(n0 << lm)));

        for (int c = 0; c < channels; ++c) {
            float prev1 = prev1_log_e[c * kCeltBands + i];
            float prev2 = prev2_log_e[c * kCeltBands + i];
            if (channels == 1) {
                prev1 = std::max(prev1, prev1_log_e[kCeltBands + i]);
                prev2 = std::max(prev2, prev2_log_e[kCeltBands + i]);
            }
            const float ediff = std::max(0.0f, log_e[c * kCeltBands + i] - std::min(prev1, prev2));

            // Short blocks do not have the same energy as long ones.
            float r = 2.0f * exp2(-ediff);
            if (lm == 3) {
                r *= 1.41421356f;
            }
            r = std::min(thresh, r);
            r *= sqrt_1;

            float* band = x + c * size + (kCeltEBands[i] << lm);
            bool renormalize = false;
            for (int k = 0; k < m; ++k) {
                if ((collapse_masks[i * channels + c] & (1 << k)) != 0) {
                    continue;
                }
                for (int j = 0; j < n0; ++j) {
                    seed = celtLcgRand(seed);
                    band[(j << lm) + k] = (seed & 0x8000) ? r : -r;
                }
                renormalize = true;
            }
            if (renormalize) {
                renormaliseVector(band, n0 << lm, 1.0f);
            }
        }
    }
}

}  // namespace player::audio::opus
