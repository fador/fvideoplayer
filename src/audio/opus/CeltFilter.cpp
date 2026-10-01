#include "audio/opus/CeltFilter.hpp"

#include "audio/opus/OpusTables.hpp"

namespace player::audio::opus {
namespace {

// Preset tap gains per tapset (celt.c).
constexpr float kCombGains[3][3] = {
    {0.3066406250f, 0.2170410156f, 0.1296386719f},
    {0.4638671875f, 0.2680664062f, 0.0f},
    {0.7998046875f, 0.1000976562f, 0.0f},
};

// Constant pitch filter over the rest of the frame.
void combFilterConst(float* y, const float* x, int t, int n, float g10, float g11, float g12) {
    float x4 = x[-t - 2];
    float x3 = x[-t - 1];
    float x2 = x[-t];
    float x1 = x[-t + 1];
    for (int i = 0; i < n; ++i) {
        const float x0 = x[i - t + 2];
        y[i] = x[i] + g10 * x2 + g11 * (x1 + x3) + g12 * (x0 + x4);
        x4 = x3;
        x3 = x2;
        x2 = x1;
        x1 = x0;
    }
}

}  // namespace

void combFilter(float* y, float* x, int t0, int t1, int n, float g0, float g1, int tapset0,
                int tapset1, const float* window, int overlap) {
    if (g0 == 0 && g1 == 0) {
        if (x != y) {
            for (int i = 0; i < n; ++i) {
                y[i] = x[i];
            }
        }
        return;
    }

    // Zero gains carry period zero; keep the history accesses in range.
    if (t0 < kCombFilterMinPeriod) {
        t0 = kCombFilterMinPeriod;
    }
    if (t1 < kCombFilterMinPeriod) {
        t1 = kCombFilterMinPeriod;
    }

    const float g00 = g0 * kCombGains[tapset0][0];
    const float g01 = g0 * kCombGains[tapset0][1];
    const float g02 = g0 * kCombGains[tapset0][2];
    const float g10 = g1 * kCombGains[tapset1][0];
    const float g11 = g1 * kCombGains[tapset1][1];
    const float g12 = g1 * kCombGains[tapset1][2];

    float x1 = x[-t1 + 1];
    float x2 = x[-t1];
    float x3 = x[-t1 - 1];
    float x4 = x[-t1 - 2];

    // No crossfade when the filter did not change.
    if (g0 == g1 && t0 == t1 && tapset0 == tapset1) {
        overlap = 0;
    }

    int i = 0;
    for (; i < overlap; ++i) {
        const float x0 = x[i - t1 + 2];
        const float f = window[i] * window[i];
        y[i] = x[i] + ((1.0f - f) * g00) * x[i - t0] +
               ((1.0f - f) * g01) * (x[i - t0 + 1] + x[i - t0 - 1]) +
               ((1.0f - f) * g02) * (x[i - t0 + 2] + x[i - t0 - 2]) + (f * g10) * x2 +
               (f * g11) * (x1 + x3) + (f * g12) * (x0 + x4);
        x4 = x3;
        x3 = x2;
        x2 = x1;
        x1 = x0;
    }

    if (g1 == 0) {
        if (x != y) {
            for (; i < n; ++i) {
                y[i] = x[i];
            }
        }
        return;
    }

    combFilterConst(y + i, x + i, t1, n - i, g10, g11, g12);
}

void deemphasis(float* const in[], float* pcm, int n, int channels, int downsample, float coef0,
                float* mem) {
    constexpr float kVerySmall = 1e-30f;
    constexpr float kScale = 1.0f / kCeltSigScale;

    for (int c = 0; c < channels; ++c) {
        const float* x = in[c];
        float* y = pcm + c;
        float m = mem[c];
        if (downsample == 1) {
            for (int j = 0; j < n; ++j) {
                const float tmp = x[j] + kVerySmall + m;
                m = coef0 * tmp;
                y[j * channels] = tmp * kScale;
            }
        } else {
            for (int j = 0; j < n; ++j) {
                const float tmp = x[j] + kVerySmall + m;
                m = coef0 * tmp;
                if (j % downsample == 0) {
                    y[(j / downsample) * channels] = tmp * kScale;
                }
            }
        }
        mem[c] = m;
    }
}

}  // namespace player::audio::opus
