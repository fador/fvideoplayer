#include "audio/opus/CeltLpc.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace player::audio::opus {
namespace {

// 4-way dot product kernel (xcorr_kernel_c). The accumulation order is kept
// identical to the reference so the float results are bit-exact.
void xcorrKernel(const float* x, const float* y, float sum[4], int len) {
    float y0 = *y++;
    float y1 = *y++;
    float y2 = *y++;
    float y3 = 0;

    int j;
    for (j = 0; j < len - 3; j += 4) {
        float tmp = *x++;
        y3 = *y++;
        sum[0] += tmp * y0;
        sum[1] += tmp * y1;
        sum[2] += tmp * y2;
        sum[3] += tmp * y3;

        tmp = *x++;
        y0 = *y++;
        sum[0] += tmp * y1;
        sum[1] += tmp * y2;
        sum[2] += tmp * y3;
        sum[3] += tmp * y0;

        tmp = *x++;
        y1 = *y++;
        sum[0] += tmp * y2;
        sum[1] += tmp * y3;
        sum[2] += tmp * y0;
        sum[3] += tmp * y1;

        tmp = *x++;
        y2 = *y++;
        sum[0] += tmp * y3;
        sum[1] += tmp * y0;
        sum[2] += tmp * y1;
        sum[3] += tmp * y2;
    }

    if (j++ < len) {
        const float tmp = *x++;
        y3 = *y++;
        sum[0] += tmp * y0;
        sum[1] += tmp * y1;
        sum[2] += tmp * y2;
        sum[3] += tmp * y3;
    }
    if (j++ < len) {
        const float tmp = *x++;
        y0 = *y++;
        sum[0] += tmp * y1;
        sum[1] += tmp * y2;
        sum[2] += tmp * y3;
        sum[3] += tmp * y0;
    }
    if (j < len) {
        const float tmp = *x++;
        y1 = *y++;
        sum[0] += tmp * y2;
        sum[1] += tmp * y3;
        sum[2] += tmp * y0;
        sum[3] += tmp * y1;
    }
}

// Finds the two best pitch candidates for a correlation vector.
void findBestPitch(const float* xcorr, const float* y, int len, int max_pitch, int* best_pitch) {
    float best_num[2] = {-1, -1};
    float best_den[2] = {0, 0};
    best_pitch[0] = 0;
    best_pitch[1] = 1;

    float syy = 1;
    for (int j = 0; j < len; ++j) {
        syy += y[j] * y[j];
    }

    for (int i = 0; i < max_pitch; ++i) {
        if (xcorr[i] > 0) {
            float xcorr16 = xcorr[i] * 1e-12f;
            const float num = xcorr16 * xcorr16;
            if (num * best_den[1] > best_num[1] * syy) {
                if (num * best_den[0] > best_num[0] * syy) {
                    best_num[1] = best_num[0];
                    best_den[1] = best_den[0];
                    best_pitch[1] = best_pitch[0];
                    best_num[0] = num;
                    best_den[0] = syy;
                    best_pitch[0] = i;
                } else {
                    best_num[1] = num;
                    best_den[1] = syy;
                    best_pitch[1] = i;
                }
            }
        }
        syy += y[i + len] * y[i + len] - y[i] * y[i];
        syy = std::max(1.0f, syy);
    }
}

// 5-tap FIR with zero initial state (celt_fir5), used by the downsample.
void celtFir5(float* x, const float* num, int n) {
    const float num0 = num[0];
    const float num1 = num[1];
    const float num2 = num[2];
    const float num3 = num[3];
    const float num4 = num[4];
    float mem0 = 0;
    float mem1 = 0;
    float mem2 = 0;
    float mem3 = 0;
    float mem4 = 0;

    for (int i = 0; i < n; ++i) {
        float sum = x[i];
        sum += num0 * mem0;
        sum += num1 * mem1;
        sum += num2 * mem2;
        sum += num3 * mem3;
        sum += num4 * mem4;
        mem4 = mem3;
        mem3 = mem2;
        mem2 = mem1;
        mem1 = mem0;
        mem0 = x[i];
        x[i] = sum;
    }
}

// Decimates the decoder history by 2 and whitens it with a 4th order LPC
// filter (pitch_downsample).
void pitchDownsample(float* const x[2], float* x_lp, int len, int channels) {
    for (int i = 1; i < len >> 1; ++i) {
        x_lp[i] = 0.5f * (0.5f * (x[0][2 * i - 1] + x[0][2 * i + 1]) + x[0][2 * i]);
    }
    x_lp[0] = 0.5f * (0.5f * x[0][1] + x[0][0]);

    if (channels == 2) {
        for (int i = 1; i < len >> 1; ++i) {
            x_lp[i] += 0.5f * (0.5f * (x[1][2 * i - 1] + x[1][2 * i + 1]) + x[1][2 * i]);
        }
        x_lp[0] += 0.5f * (0.5f * x[1][1] + x[1][0]);
    }

    float ac[5];
    celtAutocorr(x_lp, ac, nullptr, 0, 4, len >> 1);
    ac[0] *= 1.0001f;
    for (int i = 1; i <= 4; ++i) {
        ac[i] -= ac[i] * (0.008f * i) * (0.008f * i);
    }

    float lpc[4];
    celtLpc(lpc, ac, 4);
    float tmp = 1.0f;
    for (int i = 0; i < 4; ++i) {
        tmp *= 0.9f;
        lpc[i] *= tmp;
    }

    // Add a zero at 0.8 to compensate for the decimation.
    const float lpc2[5] = {
        lpc[0] + 0.8f,
        lpc[1] + 0.8f * lpc[0],
        lpc[2] + 0.8f * lpc[1],
        lpc[3] + 0.8f * lpc[2],
        0.8f * lpc[3],
    };
    celtFir5(x_lp, lpc2, len >> 1);
}

// Two-stage pitch search (coarse at 4x decimation, fine at 2x).
void pitchSearch(const float* x_lp, const float* y, int len, int max_pitch, int* pitch) {
    const int lag = len + max_pitch;
    std::vector<float> x_lp4(len >> 2);
    std::vector<float> y_lp4(lag >> 2);
    std::vector<float> xcorr(max_pitch >> 1);

    for (int j = 0; j < (len >> 2); ++j) {
        x_lp4[j] = x_lp[2 * j];
    }
    for (int j = 0; j < (lag >> 2); ++j) {
        y_lp4[j] = y[2 * j];
    }

    int best_pitch[2] = {0, 0};
    celtPitchXcorr(x_lp4.data(), y_lp4.data(), xcorr.data(), len >> 2, max_pitch >> 2);
    findBestPitch(xcorr.data(), y_lp4.data(), len >> 2, max_pitch >> 2, best_pitch);

    for (int i = 0; i < (max_pitch >> 1); ++i) {
        xcorr[i] = 0;
        if (std::abs(i - 2 * best_pitch[0]) > 2 && std::abs(i - 2 * best_pitch[1]) > 2) {
            continue;
        }
        xcorr[i] = std::max(-1.0f, celtInnerProd(x_lp, y + i, len >> 1));
    }
    findBestPitch(xcorr.data(), y, len >> 1, max_pitch >> 1, best_pitch);

    int offset;
    if (best_pitch[0] > 0 && best_pitch[0] < (max_pitch >> 1) - 1) {
        const float a = xcorr[best_pitch[0] - 1];
        const float b = xcorr[best_pitch[0]];
        const float c = xcorr[best_pitch[0] + 1];
        if ((c - a) > 0.7f * (b - a)) {
            offset = 1;
        } else if ((a - c) > 0.7f * (b - c)) {
            offset = -1;
        } else {
            offset = 0;
        }
    } else {
        offset = 0;
    }
    *pitch = 2 * best_pitch[0] - offset;
}

}  // namespace

void celtAutocorr(const float* x, float* ac, const float* window, int overlap, int lag, int n) {
    const int fast_n = n - lag;
    std::vector<float> xx;
    const float* xptr = x;

    if (overlap != 0) {
        xx.assign(x, x + n);
        for (int i = 0; i < overlap; ++i) {
            xx[i] = x[i] * window[i];
            xx[n - i - 1] = x[n - i - 1] * window[i];
        }
        xptr = xx.data();
    }

    celtPitchXcorr(xptr, xptr, ac, fast_n, lag + 1);
    for (int k = 0; k <= lag; ++k) {
        float d = 0;
        for (int i = k + fast_n; i < n; ++i) {
            d += xptr[i] * xptr[i - k];
        }
        ac[k] += d;
    }
}

void celtLpc(float* lpc, const float* ac, int p) {
    std::fill(lpc, lpc + p, 0.0f);
    if (ac[0] == 0) {
        return;
    }

    float error = ac[0];
    for (int i = 0; i < p; ++i) {
        float rr = 0;
        for (int j = 0; j < i; ++j) {
            rr += lpc[j] * ac[i - j];
        }
        rr += ac[i + 1];
        const float r = -rr / error;
        lpc[i] = r;

        for (int j = 0; j < (i + 1) >> 1; ++j) {
            const float tmp1 = lpc[j];
            const float tmp2 = lpc[i - 1 - j];
            lpc[j] = tmp1 + r * tmp2;
            lpc[i - 1 - j] = tmp2 + r * tmp1;
        }

        error = error - r * r * error;
        if (error < 0.001f * ac[0]) {
            break;
        }
    }
}

void celtFir(const float* x, const float* num, float* y, int n, int ord) {
    std::vector<float> rnum(ord);
    for (int i = 0; i < ord; ++i) {
        rnum[i] = num[ord - i - 1];
    }

    int i;
    for (i = 0; i < n - 3; i += 4) {
        float sum[4];
        sum[0] = x[i];
        sum[1] = x[i + 1];
        sum[2] = x[i + 2];
        sum[3] = x[i + 3];
        xcorrKernel(rnum.data(), x + i - ord, sum, ord);
        y[i] = sum[0];
        y[i + 1] = sum[1];
        y[i + 2] = sum[2];
        y[i + 3] = sum[3];
    }
    for (; i < n; ++i) {
        float sum = x[i];
        for (int j = 0; j < ord; ++j) {
            sum += rnum[j] * x[i + j - ord];
        }
        y[i] = sum;
    }
}

void celtIir(const float* x, const float* den, float* y, int n, int ord, float* mem) {
    std::vector<float> rden(ord);
    std::vector<float> tmp(n + ord);
    for (int i = 0; i < ord; ++i) {
        rden[i] = den[ord - i - 1];
    }

    int i;
    for (i = 0; i < ord; ++i) {
        tmp[i] = -mem[ord - i - 1];
    }
    for (; i < n + ord; ++i) {
        tmp[i] = 0;
    }

    for (i = 0; i < n - 3; i += 4) {
        float sum[4];
        sum[0] = x[i];
        sum[1] = x[i + 1];
        sum[2] = x[i + 2];
        sum[3] = x[i + 3];
        xcorrKernel(rden.data(), tmp.data() + i, sum, ord);

        // The kernel adds den*y while the recursion needs to subtract it;
        // the stored values are negated to compensate.
        tmp[i + ord] = -sum[0];
        y[i] = sum[0];
        sum[1] = sum[1] + tmp[i + ord] * den[0];
        tmp[i + ord + 1] = -sum[1];
        y[i + 1] = sum[1];
        sum[2] = sum[2] + tmp[i + ord + 1] * den[0];
        sum[2] = sum[2] + tmp[i + ord] * den[1];
        tmp[i + ord + 2] = -sum[2];
        y[i + 2] = sum[2];
        sum[3] = sum[3] + tmp[i + ord + 2] * den[0];
        sum[3] = sum[3] + tmp[i + ord + 1] * den[1];
        sum[3] = sum[3] + tmp[i + ord] * den[2];
        tmp[i + ord + 3] = -sum[3];
        y[i + 3] = sum[3];
    }
    for (; i < n; ++i) {
        float sum = x[i];
        for (int j = 0; j < ord; ++j) {
            sum -= rden[j] * tmp[i + j];
        }
        tmp[i + ord] = sum;
        y[i] = sum;
    }

    for (int k = 0; k < ord; ++k) {
        mem[k] = y[n - k - 1];
    }
}

float celtInnerProd(const float* x, const float* y, int n) {
    float xy = 0;
    for (int i = 0; i < n; ++i) {
        xy += x[i] * y[i];
    }
    return xy;
}

void celtPitchXcorr(const float* x, const float* y, float* xcorr, int len, int max_pitch) {
    int i;
    for (i = 0; i < max_pitch - 3; i += 4) {
        float sum[4] = {0, 0, 0, 0};
        xcorrKernel(x, y + i, sum, len);
        xcorr[i] = sum[0];
        xcorr[i + 1] = sum[1];
        xcorr[i + 2] = sum[2];
        xcorr[i + 3] = sum[3];
    }
    for (; i < max_pitch; ++i) {
        xcorr[i] = celtInnerProd(x, y + i, len);
    }
}

int plcPitchSearch(float* const decode_mem[2], int channels) {
    std::vector<float> lp(kCeltDecodeBufferSize >> 1);
    pitchDownsample(decode_mem, lp.data(), kCeltDecodeBufferSize, channels);

    int pitch_index = 0;
    pitchSearch(lp.data() + (kPlcPitchLagMax >> 1), lp.data(),
                kCeltDecodeBufferSize - kPlcPitchLagMax, kPlcPitchLagMax - kPlcPitchLagMin,
                &pitch_index);
    return kPlcPitchLagMax - pitch_index;
}

}  // namespace player::audio::opus
