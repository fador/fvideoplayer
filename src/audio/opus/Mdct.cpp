#include "audio/opus/Mdct.hpp"

#include <cmath>

namespace player::audio::opus {

void MdctLookup::init(int n, int max_shift) {
    n_ = n;
    max_shift_ = max_shift;

    // The FFT states get smaller while sharing the largest twiddle table.
    for (int i = 0; i <= max_shift; ++i) {
        fft_[i].init(n >> 2 >> i, i == 0 ? nullptr : &fft_[0]);
    }

    // Pre-rotation table, one half-size block per shift.
    int total = 0;
    for (int s = 0, half = n >> 1; s <= max_shift; ++s, half >>= 1) {
        total += half;
    }
    trig_.assign(kCeltMdctTwiddles, kCeltMdctTwiddles + total);
}

void MdctLookup::backward(const float* in, float* out, const float* window, int overlap, int shift,
                          int stride) const {
    int n = n_;
    const float* trig = trig_.data();
    for (int i = 0; i < shift; ++i) {
        n >>= 1;
        trig += n;
    }
    const int n2 = n >> 1;
    const int n4 = n >> 2;

    // Pre-rotate into bit-reversed order, swapping real and imaginary because
    // an FFT is used instead of an IFFT.
    const int16_t* bitrev = fft_[shift].bitrev();
    const float* xp1 = in;
    const float* xp2 = in + stride * (n2 - 1);
    float* yp = out + (overlap >> 1);
    for (int i = 0; i < n4; ++i) {
        const int rev = bitrev[i];
        const float yr = xp2[0] * trig[i] + xp1[0] * trig[n4 + i];
        const float yi = xp1[0] * trig[i] - xp2[0] * trig[n4 + i];
        yp[2 * rev + 1] = yr;
        yp[2 * rev] = yi;
        xp1 += 2 * stride;
        xp2 -= 2 * stride;
    }

    fft_[shift].transform(reinterpret_cast<Complex*>(out + (overlap >> 1)));

    // Post-rotate from both ends of the buffer at once.
    float* yp0 = out + (overlap >> 1);
    float* yp1 = out + (overlap >> 1) + n2 - 2;
    const float* t = trig;
    for (int i = 0; i < (n4 + 1) >> 1; ++i) {
        float re = yp0[1];
        float im = yp0[0];
        float t0 = t[i];
        float t1 = t[n4 + i];
        float yr = re * t0 + im * t1;
        float yi = re * t1 - im * t0;

        re = yp1[1];
        im = yp1[0];
        yp0[0] = yr;
        yp1[1] = yi;

        t0 = t[n4 - i - 1];
        t1 = t[n2 - i - 1];
        yr = re * t0 + im * t1;
        yi = re * t1 - im * t0;
        yp1[0] = yr;
        yp0[1] = yi;

        yp0 += 2;
        yp1 -= 2;
    }

    // Mirror on both sides for TDAC.
    float* xp1m = out + overlap - 1;
    float* yp1m = out;
    const float* wp1 = window;
    const float* wp2 = window + overlap - 1;
    for (int i = 0; i < overlap / 2; ++i) {
        const float x1 = *xp1m;
        const float x2 = *yp1m;
        *yp1m++ = *wp2 * x2 - *wp1 * x1;
        *xp1m-- = *wp1 * x2 + *wp2 * x1;
        ++wp1;
        --wp2;
    }
}

}  // namespace player::audio::opus
