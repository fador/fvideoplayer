#include "audio/opus/CeltVq.hpp"

#include <cmath>

namespace player::audio::opus {
namespace {

constexpr float kPi = 3.14159265358979323846f;

// cos(pi/2 * x): libopus celt_cos_norm.
float cosNorm(float x) {
    return std::cos(0.5f * kPi * x);
}

// More aggressive spreading uses a smaller factor (vq.c SPREAD_FACTOR).
constexpr int kSpreadFactor[3] = {15, 10, 5};

// One pass of the pairwise rotation; stride 1 rotates neighbors, larger
// strides rotate across the band. See libopus exp_rotation1.
void expRotation1(float* x, int len, int stride, float c, float s) {
    for (int i = 0; i < len - stride; ++i) {
        const float x1 = x[i];
        const float x2 = x[i + stride];
        x[i + stride] = c * x2 + s * x1;
        x[i] = c * x1 - s * x2;
    }
    for (int i = len - 2 * stride - 1; i >= 0; --i) {
        const float x1 = x[i];
        const float x2 = x[i + stride];
        x[i + stride] = c * x2 + s * x1;
        x[i] = c * x1 - s * x2;
    }
}

}  // namespace

void expRotation(float* x, int len, int dir, int stride, int k, Spread spread) {
    // No rotation when the band is too short or spreading is disabled.
    if (2 * k >= len || spread == Spread::None) {
        return;
    }

    const int factor = kSpreadFactor[static_cast<int>(spread) - 1];
    const float gain = static_cast<float>(len) / static_cast<float>(len + factor * k);
    const float theta = 0.5f * gain * gain;

    // c = cos(theta), s = sin(theta), computed through cos(pi/2 - theta).
    const float c = cosNorm(theta);
    const float s = cosNorm(1.0f - theta);

    // Secondary stride used on long bands: sqrt(len/stride) rounded up.
    int stride2 = 0;
    if (len >= 8 * stride) {
        stride2 = 1;
        while ((stride2 * stride2 + stride2) * stride + (stride >> 2) < len) {
            ++stride2;
        }
    }

    const int sub_len = len / stride;
    for (int i = 0; i < stride; ++i) {
        float* sub = x + i * sub_len;
        if (dir < 0) {
            if (stride2 != 0) {
                expRotation1(sub, sub_len, stride2, s, c);
            }
            expRotation1(sub, sub_len, 1, c, s);
        } else {
            expRotation1(sub, sub_len, 1, c, -s);
            if (stride2 != 0) {
                expRotation1(sub, sub_len, stride2, s, -c);
            }
        }
    }
}

void renormaliseVector(float* x, int n, float gain) {
    constexpr float kEpsilon = 1e-15f;

    float energy = kEpsilon;
    for (int i = 0; i < n; ++i) {
        energy += x[i] * x[i];
    }

    const float g = (1.0f / std::sqrt(energy)) * gain;
    for (int i = 0; i < n; ++i) {
        x[i] = g * x[i];
    }
}

}  // namespace player::audio::opus
