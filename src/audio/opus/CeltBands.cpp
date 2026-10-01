#include "audio/opus/CeltBands.hpp"

#include "audio/opus/CeltPvq.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace player::audio::opus {
namespace {

constexpr int kQthetaOffset = 4;
constexpr int kQthetaOffsetTwoPhase = 16;
constexpr int kLogMaxPseudo = 6;
constexpr float kQ15One = 1.0f;

// Largest band size: (kCeltEBands[20] - kCeltEBands[19]) << kCeltMaxLm.
constexpr int kMaxBandSize = 176;

// --- Integer helpers, copied bit-exactly from the reference. bitexactCos and
// --- bitexactLog2tan feed the bit allocation, so they cannot be approximated.

int ecIlog(uint32_t x) {
    int n = 0;
    while (x != 0) {
        ++n;
        x >>= 1;
    }
    return n;
}

int32_t fracMul16(int a, int b) {
    return (16384 + static_cast<int32_t>(static_cast<int16_t>(a)) * static_cast<int16_t>(b)) >> 15;
}

int bitexactCos(int x) {
    const int32_t tmp = (4096 + x * x) >> 13;
    int16_t x2 = static_cast<int16_t>(tmp);
    x2 = static_cast<int16_t>(static_cast<int>(32767 - x2) +
                              fracMul16(x2, -7651 + fracMul16(x2, 8277 + fracMul16(-626, x2))));
    return 1 + x2;
}

int bitexactLog2tan(int isin, int icos) {
    const int lc = ecIlog(static_cast<uint32_t>(icos));
    const int ls = ecIlog(static_cast<uint32_t>(isin));
    icos <<= 15 - lc;
    isin <<= 15 - ls;
    return (ls - lc) * (1 << 11) + fracMul16(isin, fracMul16(isin, -2597) + 7932) -
           fracMul16(icos, fracMul16(icos, -2597) + 7932);
}

uint32_t isqrt32(uint32_t val) {
    uint32_t g = 0;
    int bshift = (ecIlog(val) - 1) >> 1;
    uint32_t b = 1u << bshift;
    do {
        const uint32_t t = ((g << 1) + b) << bshift;
        if (t <= val) {
            g += b;
            val -= t;
        }
        b >>= 1;
        --bshift;
    } while (bshift >= 0);
    return g;
}

uint32_t celtLcgRand(uint32_t seed) {
    return 1664525u * seed + 1013904223u;
}

int celtUdiv(int32_t n, int32_t d) {
    return static_cast<int>(static_cast<uint32_t>(n) / static_cast<uint32_t>(d));
}

float rsqrt(float x) {
    return 1.0f / static_cast<float>(std::sqrt(x));
}

int getPulses(int i) {
    return i < 8 ? i : (8 + (i & 7)) << ((i >> 3) - 1);
}

// Pulse count for a bit budget, and the reverse, from the mode pulse cache.
int bits2Pulses(int band, int lm, int bits) {
    const uint8_t* cache = kCeltCacheBits + kCeltCacheIndex[(lm + 1) * kCeltBands + band];
    int lo = 0;
    int hi = cache[0];
    --bits;
    for (int i = 0; i < kLogMaxPseudo; ++i) {
        const int mid = (lo + hi + 1) >> 1;
        if (static_cast<int>(cache[mid]) >= bits) {
            hi = mid;
        } else {
            lo = mid;
        }
    }
    const int lo_bits = lo == 0 ? -1 : static_cast<int>(cache[lo]);
    return bits - lo_bits <= static_cast<int>(cache[hi]) - bits ? lo : hi;
}

int pulses2Bits(int band, int lm, int pulses) {
    if (pulses == 0) {
        return 0;
    }
    const uint8_t* cache = kCeltCacheBits + kCeltCacheIndex[(lm + 1) * kCeltBands + band];
    return cache[pulses] + 1;
}

// --- Band transforms and helpers ---

void haar1(float* x, int n0, int stride) {
    n0 >>= 1;
    for (int i = 0; i < stride; ++i) {
        for (int j = 0; j < n0; ++j) {
            const float tmp1 = 0.70710678f * x[stride * 2 * j + i];
            const float tmp2 = 0.70710678f * x[stride * (2 * j + 1) + i];
            x[stride * 2 * j + i] = tmp1 + tmp2;
            x[stride * (2 * j + 1) + i] = tmp1 - tmp2;
        }
    }
}

void deinterleaveHadamard(float* x, int n0, int stride, int hadamard) {
    const int n = n0 * stride;
    float tmp[kMaxBandSize];
    if (hadamard) {
        const int16_t* ordery = kCeltOrdery + stride - 2;
        for (int i = 0; i < stride; ++i) {
            for (int j = 0; j < n0; ++j) {
                tmp[ordery[i] * n0 + j] = x[j * stride + i];
            }
        }
    } else {
        for (int i = 0; i < stride; ++i) {
            for (int j = 0; j < n0; ++j) {
                tmp[i * n0 + j] = x[j * stride + i];
            }
        }
    }
    std::copy(tmp, tmp + n, x);
}

void interleaveHadamard(float* x, int n0, int stride, int hadamard) {
    const int n = n0 * stride;
    float tmp[kMaxBandSize];
    if (hadamard) {
        const int16_t* ordery = kCeltOrdery + stride - 2;
        for (int i = 0; i < stride; ++i) {
            for (int j = 0; j < n0; ++j) {
                tmp[j * stride + i] = x[ordery[i] * n0 + j];
            }
        }
    } else {
        for (int i = 0; i < stride; ++i) {
            for (int j = 0; j < n0; ++j) {
                tmp[j * stride + i] = x[i * n0 + j];
            }
        }
    }
    std::copy(tmp, tmp + n, x);
}

// Duplicates the folding data needed by the second band; a no-op for
// CELT-only modes where the first two bands have equal width.
void specialHybridFolding(float* norm, float* norm2, int start, int m, int dual_stereo) {
    const int n1 = m * (kCeltEBands[start + 1] - kCeltEBands[start]);
    const int n2 = m * (kCeltEBands[start + 2] - kCeltEBands[start + 1]);
    const int count = n2 - n1;
    if (count <= 0) {
        return;
    }
    std::copy(norm + 2 * n1 - n2, norm + 2 * n1 - n2 + count, norm + n1);
    if (dual_stereo) {
        std::copy(norm2 + 2 * n1 - n2, norm2 + 2 * n1 - n2 + count, norm2 + n1);
    }
}

void normaliseResidual(const int* iy, float* x, int n, float ryy, float gain) {
    const float g = rsqrt(ryy) * gain;
    for (int i = 0; i < n; ++i) {
        x[i] = g * iy[i];
    }
}

unsigned extractCollapseMask(const int* iy, int n, int blocks) {
    if (blocks <= 1) {
        return 1;
    }
    const int n0 = celtUdiv(n, blocks);
    unsigned mask = 0;
    for (int i = 0; i < blocks; ++i) {
        unsigned tmp = 0;
        for (int j = 0; j < n0; ++j) {
            tmp |= static_cast<unsigned>(iy[i * n0 + j]);
        }
        mask |= (tmp != 0 ? 1u : 0u) << i;
    }
    return mask;
}

// PVQ decoding of one band, including spreading rotation (vq.c alg_unquant).
unsigned algUnquant(RangeDecoder& decoder, float* x, int n, int k, Spread spread, int blocks,
                    float gain) {
    int iy[kMaxBandSize];
    const float ryy = decodePulses(decoder, n, k, iy);
    normaliseResidual(iy, x, n, ryy, gain);
    expRotation(x, n, -1, blocks, k, spread);
    return extractCollapseMask(iy, n, blocks);
}

// Recombines the mid and side of a stereo band (bands.c stereo_merge).
void stereoMerge(float* x, float* y, float mid, int n) {
    float xp = 0;
    float side = 0;
    for (int j = 0; j < n; ++j) {
        xp += y[j] * x[j];
        side += y[j] * y[j];
    }
    xp *= mid;

    const float mid2 = mid;
    const float el = mid2 * mid2 + side - 2 * xp;
    const float er = mid2 * mid2 + side + 2 * xp;
    if (er < 6e-4f || el < 6e-4f) {
        std::copy(x, x + n, y);
        return;
    }

    const float lgain = rsqrt(el);
    const float rgain = rsqrt(er);
    for (int j = 0; j < n; ++j) {
        const float l = mid * x[j];
        const float r = y[j];
        x[j] = lgain * (l - r);
        y[j] = rgain * (l + r);
    }
}

struct BandCtx {
    int i = 0;
    int intensity = 0;
    Spread spread = Spread::Normal;
    int tf_change = 0;
    int32_t remaining_bits = 0;
    int disable_inv = 0;
    uint32_t seed = 0;
    RangeDecoder* decoder = nullptr;
};

struct SplitCtx {
    int inv = 0;
    int imid = 0;
    int iside = 0;
    int delta = 0;
    int itheta = 0;
    int qalloc = 0;
};

// Number of theta quantization levels for a band (bands.c compute_qn).
int computeQn(int n, int b, int offset, int pulse_cap, int stereo) {
    static const int16_t kExp2Table8[8] = {16384, 17866, 19483, 21247, 23170, 25267, 27554, 30048};
    int n2 = 2 * n - 1;
    if (stereo && n == 2) {
        --n2;
    }
    int qb = (b + n2 * offset) / n2;
    qb = std::min(b - pulse_cap - (4 << kCeltBitsRes), qb);
    qb = std::min(8 << kCeltBitsRes, qb);
    if (qb < (1 << kCeltBitsRes >> 1)) {
        return 1;
    }
    const int qn = kExp2Table8[qb & 0x7] >> (14 - (qb >> kCeltBitsRes));
    return ((qn + 1) >> 1) << 1;
}

// Decodes the angle between the two halves of a split (bands.c compute_theta,
// decoder side).
SplitCtx computeTheta(BandCtx& ctx, int n, int& b, int blocks, int b0, int lm, int stereo,
                      int& fill) {
    const int pulse_cap = kCeltLogN[ctx.i] + lm * (1 << kCeltBitsRes);
    const int offset = (pulse_cap >> 1) - (stereo && n == 2 ? kQthetaOffsetTwoPhase : kQthetaOffset);
    int qn = computeQn(n, b, offset, pulse_cap, stereo);
    if (stereo && ctx.i >= ctx.intensity) {
        qn = 1;
    }

    const int tell = ctx.decoder->tellFrac();
    int itheta = 0;
    int inv = 0;
    if (qn != 1) {
        if (stereo && n > 2) {
            // Step probability: p0 up to the middle, then uniform.
            const int p0 = 3;
            const int x0 = qn / 2;
            const int ft = p0 * (x0 + 1) + x0;
            const int fs = static_cast<int>(ctx.decoder->decode(static_cast<uint32_t>(ft)));
            int x;
            if (fs < (x0 + 1) * p0) {
                x = fs / p0;
            } else {
                x = x0 + 1 + (fs - (x0 + 1) * p0);
            }
            const int low = x <= x0 ? p0 * x : (x - 1 - x0) + (x0 + 1) * p0;
            const int high = x <= x0 ? p0 * (x + 1) : (x - x0) + (x0 + 1) * p0;
            ctx.decoder->update(static_cast<uint32_t>(low), static_cast<uint32_t>(high),
                                static_cast<uint32_t>(ft));
            itheta = x;
        } else if (b0 > 1 || stereo) {
            itheta = static_cast<int>(ctx.decoder->decodeUint(static_cast<uint32_t>(qn + 1)));
        } else {
            // Triangular probability.
            const int half = qn >> 1;
            const int ft = (half + 1) * (half + 1);
            const int fm = static_cast<int>(ctx.decoder->decode(static_cast<uint32_t>(ft)));
            int fs;
            int fl;
            if (fm < (half * (half + 1) >> 1)) {
                itheta = (static_cast<int>(isqrt32(8u * static_cast<uint32_t>(fm) + 1)) - 1) >> 1;
                fs = itheta + 1;
                fl = itheta * (itheta + 1) >> 1;
            } else {
                itheta = (2 * (qn + 1) -
                          static_cast<int>(isqrt32(8u * static_cast<uint32_t>(ft - fm - 1) + 1))) >>
                         1;
                fs = qn + 1 - itheta;
                fl = ft - ((qn + 1 - itheta) * (qn + 2 - itheta) >> 1);
            }
            ctx.decoder->update(static_cast<uint32_t>(fl), static_cast<uint32_t>(fl + fs),
                                static_cast<uint32_t>(ft));
        }
        itheta = celtUdiv(static_cast<int32_t>(itheta) * 16384, qn);
    } else if (stereo) {
        if (b > 2 << kCeltBitsRes && ctx.remaining_bits > 2 << kCeltBitsRes) {
            inv = ctx.decoder->decodeBitLogp(2) ? 1 : 0;
        } else {
            inv = 0;
        }
        if (ctx.disable_inv) {
            inv = 0;
        }
        itheta = 0;
    }

    const int qalloc = ctx.decoder->tellFrac() - tell;
    b -= qalloc;

    SplitCtx sctx;
    sctx.inv = inv;
    sctx.itheta = itheta;
    sctx.qalloc = qalloc;
    if (itheta == 0) {
        sctx.imid = 32767;
        sctx.iside = 0;
        sctx.delta = -16384;
        fill &= (1 << blocks) - 1;
    } else if (itheta == 16384) {
        sctx.imid = 0;
        sctx.iside = 32767;
        sctx.delta = 16384;
        fill &= ((1 << blocks) - 1) << blocks;
    } else {
        sctx.imid = bitexactCos(itheta);
        sctx.iside = bitexactCos(16384 - itheta);
        sctx.delta = fracMul16((n - 1) << 7, bitexactLog2tan(sctx.iside, sctx.imid));
    }
    return sctx;
}

unsigned quantBandN1(BandCtx& ctx, float* x, float* y, int b, float* lowband_out) {
    const int stereo = y != nullptr;
    for (int c = 0; c < 1 + stereo; ++c) {
        float* v = c == 0 ? x : y;
        int sign = 0;
        if (ctx.remaining_bits >= 1 << kCeltBitsRes) {
            sign = static_cast<int>(ctx.decoder->decodeBits(1));
            ctx.remaining_bits -= 1 << kCeltBitsRes;
            b -= 1 << kCeltBitsRes;
        }
        v[0] = sign ? -1.0f : 1.0f;
    }
    if (lowband_out) {
        lowband_out[0] = x[0];
    }
    return 1;
}

// Encodes/decodes a mono partition; may split the band in two recursively
// (bands.c quant_partition).
unsigned quantPartition(BandCtx& ctx, float* x, int n, int b, int blocks, float* lowband, int lm,
                        float gain, int fill) {
    const uint8_t* cache = kCeltCacheBits + kCeltCacheIndex[(lm + 1) * kCeltBands + ctx.i];

    // Split when the budget is more than 1.5 bits per sample above the
    // no-split cost.
    if (lm != -1 && b > static_cast<int>(cache[cache[0]]) + 12 && n > 2) {
        const int b0 = blocks;
        const int sub_n = n >> 1;
        float* y = x + sub_n;
        const int sub_lm = lm - 1;
        if (blocks == 1) {
            fill = (fill & 1) | (fill << 1);
        }
        const int sub_blocks = (blocks + 1) >> 1;

        const SplitCtx sctx = computeTheta(ctx, sub_n, b, sub_blocks, b0, sub_lm, 0, fill);
        const int itheta = sctx.itheta;
        int delta = sctx.delta;
        const float mid = (1.0f / 32768.0f) * sctx.imid;
        const float side = (1.0f / 32768.0f) * sctx.iside;

        // More bits to the low-energy half on transients and to the mid band
        // otherwise (pre-echo masking).
        if (b0 > 1 && (itheta & 0x3fff)) {
            if (itheta > 8192) {
                delta -= delta >> (4 - sub_lm);
            } else {
                delta = std::min(0, delta + (sub_n << kCeltBitsRes >> (5 - sub_lm)));
            }
        }
        int mbits = std::max(0, std::min(b, (b - delta) / 2));
        int sbits = b - mbits;
        ctx.remaining_bits -= sctx.qalloc;

        float* next_lowband2 = lowband ? lowband + sub_n : nullptr;
        int32_t rebalance = ctx.remaining_bits;
        unsigned cm;
        if (mbits >= sbits) {
            cm = quantPartition(ctx, x, sub_n, mbits, sub_blocks, lowband, sub_lm, gain * mid, fill);
            rebalance = mbits - (rebalance - ctx.remaining_bits);
            if (rebalance > 3 << kCeltBitsRes && itheta != 0) {
                sbits += rebalance - (3 << kCeltBitsRes);
            }
            cm |= quantPartition(ctx, y, sub_n, sbits, sub_blocks, next_lowband2, sub_lm,
                                 gain * side, fill >> sub_blocks)
                  << (b0 >> 1);
        } else {
            cm = quantPartition(ctx, y, sub_n, sbits, sub_blocks, next_lowband2, sub_lm, gain * side,
                                fill >> sub_blocks)
                 << (b0 >> 1);
            rebalance = sbits - (rebalance - ctx.remaining_bits);
            if (rebalance > 3 << kCeltBitsRes && itheta != 16384) {
                mbits += rebalance - (3 << kCeltBitsRes);
            }
            cm |= quantPartition(ctx, x, sub_n, mbits, sub_blocks, lowband, sub_lm, gain * mid, fill);
        }
        return cm;
    }

    // No split: one PVQ codeword, or a folded/noise fill when no pulse fits.
    int q = bits2Pulses(ctx.i, lm, b);
    int curr_bits = pulses2Bits(ctx.i, lm, q);
    ctx.remaining_bits -= curr_bits;
    while (ctx.remaining_bits < 0 && q > 0) {
        ctx.remaining_bits += curr_bits;
        --q;
        curr_bits = pulses2Bits(ctx.i, lm, q);
        ctx.remaining_bits -= curr_bits;
    }

    if (q != 0) {
        return algUnquant(*ctx.decoder, x, n, getPulses(q), ctx.spread, blocks, gain);
    }

    const unsigned cm_mask = (1u << blocks) - 1;
    fill &= static_cast<int>(cm_mask);
    if (fill == 0) {
        std::fill(x, x + n, 0.0f);
        return 0;
    }
    if (lowband == nullptr) {
        // Pure noise.
        for (int j = 0; j < n; ++j) {
            ctx.seed = celtLcgRand(ctx.seed);
            x[j] = static_cast<float>(static_cast<int32_t>(ctx.seed) >> 20);
        }
        renormaliseVector(x, n, gain);
        return cm_mask;
    }
    // Folded spectrum, about 48 dB below the normal folding level.
    for (int j = 0; j < n; ++j) {
        ctx.seed = celtLcgRand(ctx.seed);
        const float tmp = (ctx.seed & 0x8000) ? (1.0f / 256.0f) : -(1.0f / 256.0f);
        x[j] = lowband[j] + tmp;
    }
    renormaliseVector(x, n, gain);
    return static_cast<unsigned>(fill);
}

// Mono band: time/frequency resolution changes around quant_partition
// (bands.c quant_band, decoder side).
unsigned quantBand(BandCtx& ctx, float* x, int n, int b, int blocks, float* lowband, int lm,
                   float* lowband_out, float gain, float* lowband_scratch, int fill) {
    const int n0 = n;
    int n_b = n;
    int b0 = blocks;
    int time_divide = 0;
    int recombine = 0;
    const int long_blocks = b0 == 1;
    n_b = celtUdiv(n_b, blocks);

    if (n == 1) {
        return quantBandN1(ctx, x, nullptr, b, lowband_out);
    }

    int tf_change = ctx.tf_change;
    if (tf_change > 0) {
        recombine = tf_change;
    }
    if (lowband_scratch && lowband &&
        (recombine || ((n_b & 1) == 0 && tf_change < 0) || b0 > 1)) {
        std::copy(lowband, lowband + n, lowband_scratch);
        lowband = lowband_scratch;
    }

    // Increase frequency resolution by recombining time blocks.
    for (int k = 0; k < recombine; ++k) {
        static const uint8_t kBitInterleaveTable[16] = {0, 1, 1, 1, 2, 3, 3, 3,
                                                        2, 3, 3, 3, 2, 3, 3, 3};
        if (lowband) {
            haar1(lowband, n >> k, 1 << k);
        }
        fill = kBitInterleaveTable[fill & 0xF] | kBitInterleaveTable[fill >> 4] << 2;
    }
    blocks >>= recombine;
    n_b <<= recombine;

    // Increase time resolution by splitting the band in time.
    while ((n_b & 1) == 0 && tf_change < 0) {
        if (lowband) {
            haar1(lowband, n_b, blocks);
        }
        fill |= fill << blocks;
        blocks <<= 1;
        n_b >>= 1;
        ++time_divide;
        ++tf_change;
    }
    b0 = blocks;
    const int n_b0 = n_b;

    // Samples in time order instead of frequency order.
    if (b0 > 1 && lowband) {
        deinterleaveHadamard(lowband, n_b >> recombine, b0 << recombine, long_blocks);
    }

    unsigned cm = quantPartition(ctx, x, n, b, blocks, lowband, lm, gain, fill);

    // Decoder: undo the transforms and prepare the folding data.
    if (b0 > 1) {
        interleaveHadamard(x, n_b >> recombine, b0 << recombine, long_blocks);
    }
    n_b = n_b0;
    for (int k = 0; k < time_divide; ++k) {
        blocks >>= 1;
        n_b <<= 1;
        cm |= cm >> blocks;
        haar1(x, n_b, blocks);
    }
    for (int k = 0; k < recombine; ++k) {
        static const uint8_t kBitDeinterleaveTable[16] = {0x00, 0x03, 0x0C, 0x0F, 0x30, 0x33,
                                                          0x3C, 0x3F, 0xC0, 0xC3, 0xCC, 0xCF,
                                                          0xF0, 0xF3, 0xFC, 0xFF};
        cm = kBitDeinterleaveTable[cm];
        haar1(x, n0 >> k, 1 << k);
    }
    blocks <<= recombine;

    if (lowband_out) {
        const float scale = static_cast<float>(std::sqrt(static_cast<float>(n0)));
        for (int j = 0; j < n0; ++j) {
            lowband_out[j] = scale * x[j];
        }
    }
    cm &= (1u << blocks) - 1;
    return cm;
}

// Stereo band: mid/side split with the theta angle (bands.c quant_band_stereo,
// decoder side).
unsigned quantBandStereo(BandCtx& ctx, float* x, float* y, int n, int b, int blocks,
                         float* lowband, int lm, float* lowband_out, float* lowband_scratch,
                         int fill) {
    if (n == 1) {
        return quantBandN1(ctx, x, y, b, lowband_out);
    }

    const int orig_fill = fill;
    const SplitCtx sctx = computeTheta(ctx, n, b, blocks, blocks, lm, 1, fill);
    const int inv = sctx.inv;
    const int itheta = sctx.itheta;
    const float mid = (1.0f / 32768.0f) * sctx.imid;
    const float side = (1.0f / 32768.0f) * sctx.iside;

    unsigned cm;
    if (n == 2) {
        // One bit is enough for the side thanks to orthogonality.
        int sbits = itheta != 0 && itheta != 16384 ? 1 << kCeltBitsRes : 0;
        const int mbits = b - sbits;
        const int c = itheta > 8192 ? 1 : 0;
        ctx.remaining_bits -= sctx.qalloc + sbits;

        float* x2 = c ? y : x;
        float* y2 = c ? x : y;
        int sign = 0;
        if (sbits != 0) {
            sign = static_cast<int>(ctx.decoder->decodeBits(1));
        }
        sign = 1 - 2 * sign;

        cm = quantBand(ctx, x2, n, mbits, blocks, lowband, lm, lowband_out, kQ15One, lowband_scratch,
                       orig_fill);
        y2[0] = -sign * x2[1];
        y2[1] = sign * x2[0];

        float tmp = x[0];
        x[0] = mid * tmp - side * y[0];
        y[0] = mid * tmp + side * y[0];
        tmp = x[1];
        x[1] = mid * tmp - side * y[1];
        y[1] = mid * tmp + side * y[1];
    } else {
        int mbits = std::max(0, std::min(b, (b - sctx.delta) / 2));
        int sbits = b - mbits;
        ctx.remaining_bits -= sctx.qalloc;

        int32_t rebalance = ctx.remaining_bits;
        if (mbits >= sbits) {
            cm = quantBand(ctx, x, n, mbits, blocks, lowband, lm, lowband_out, kQ15One,
                           lowband_scratch, fill);
            rebalance = mbits - (rebalance - ctx.remaining_bits);
            if (rebalance > 3 << kCeltBitsRes && itheta != 0) {
                sbits += rebalance - (3 << kCeltBitsRes);
            }
            // The side never folds.
            cm |= quantBand(ctx, y, n, sbits, blocks, nullptr, lm, nullptr, side, nullptr,
                            fill >> blocks);
        } else {
            cm = quantBand(ctx, y, n, sbits, blocks, nullptr, lm, nullptr, side, nullptr,
                           fill >> blocks);
            rebalance = sbits - (rebalance - ctx.remaining_bits);
            if (rebalance > 3 << kCeltBitsRes && itheta != 16384) {
                mbits += rebalance - (3 << kCeltBitsRes);
            }
            cm |= quantBand(ctx, x, n, mbits, blocks, lowband, lm, lowband_out, kQ15One,
                            lowband_scratch, fill);
        }
        stereoMerge(x, y, mid, n);
    }

    if (inv) {
        for (int j = 0; j < n; ++j) {
            y[j] = -y[j];
        }
    }
    return cm;
}

}  // namespace

void quantAllBands(RangeDecoder& decoder, const BandsParams& params, int channels, float* x,
                   float* y, uint8_t* collapse_masks, uint32_t& seed) {
    const int m = 1 << params.lm;
    const int b_blocks = params.short_blocks ? m : 1;
    const int norm_offset = m * kCeltEBands[params.start];

    BandCtx ctx;
    ctx.decoder = &decoder;
    ctx.intensity = params.intensity;
    ctx.spread = params.spread;
    ctx.seed = seed;

    const int norm_size = m * kCeltEBands[kCeltBands - 1] - norm_offset;
    std::vector<float> norm_storage(static_cast<size_t>(channels) * norm_size, 0.0f);
    float* norm = norm_storage.data();
    float* norm2 = norm + norm_size;

    // The last band's output doubles as scratch space for the folding data.
    float* lowband_scratch = x + m * kCeltEBands[kCeltBands - 1];
    int lowband_offset = 0;
    int update_lowband = 1;
    int dual_stereo = params.dual_stereo;
    int32_t balance = params.balance;

    for (int i = params.start; i < params.end; ++i) {
        ctx.i = i;
        const int last = i == params.end - 1;
        float* bx = x + m * kCeltEBands[i];
        float* by = y ? y + m * kCeltEBands[i] : nullptr;
        const int n = m * kCeltEBands[i + 1] - m * kCeltEBands[i];

        const int tell = decoder.tellFrac();
        if (i != params.start) {
            balance -= tell;
        }
        const int32_t remaining_bits = params.total_bits - tell - 1;
        ctx.remaining_bits = remaining_bits;

        int b;
        if (i <= params.coded_bands - 1) {
            const int curr_balance =
                static_cast<int>(balance / std::min(3, params.coded_bands - i));
            b = std::max(0, std::min(16383, std::min(remaining_bits + 1, params.pulses[i] + curr_balance)));
        } else {
            b = 0;
        }

        // Pick the band to fold from: the most recent one wide enough.
        if ((m * kCeltEBands[i] - n >= m * kCeltEBands[params.start] || i == params.start + 1) &&
            (update_lowband || lowband_offset == 0)) {
            lowband_offset = i;
        }
        if (i == params.start + 1) {
            specialHybridFolding(norm, norm2, params.start, m, dual_stereo);
        }
        ctx.tf_change = params.tf_res[i];
        if (last) {
            lowband_scratch = nullptr;
        }

        int x_cm;
        int y_cm;
        int effective_lowband = -1;
        if (lowband_offset != 0 &&
            (params.spread != Spread::Aggressive || b_blocks > 1 || ctx.tf_change < 0)) {
            // Conservative estimate of the collapse masks of the folded bands.
            effective_lowband = std::max(0, m * kCeltEBands[lowband_offset] - norm_offset - n);
            int fold_start = lowband_offset;
            while (m * kCeltEBands[--fold_start] > effective_lowband + norm_offset) {
            }
            int fold_end = lowband_offset - 1;
            while (++fold_end < i &&
                   m * kCeltEBands[fold_end] < effective_lowband + norm_offset + n) {
            }
            x_cm = 0;
            y_cm = 0;
            int fold_i = fold_start;
            do {
                x_cm |= collapse_masks[fold_i * channels];
                y_cm |= collapse_masks[fold_i * channels + channels - 1];
            } while (++fold_i < fold_end);
        } else {
            x_cm = y_cm = (1 << b_blocks) - 1;
        }

        if (dual_stereo && i == params.intensity) {
            // Switch to intensity: fold the two channels into one.
            dual_stereo = 0;
            for (int j = 0; j < m * kCeltEBands[i] - norm_offset; ++j) {
                norm[j] = 0.5f * (norm[j] + norm2[j]);
            }
        }

        if (dual_stereo) {
            x_cm = static_cast<int>(quantBand(
                ctx, bx, n, b / 2, b_blocks,
                effective_lowband != -1 ? norm + effective_lowband : nullptr, params.lm,
                last ? nullptr : norm + m * kCeltEBands[i] - norm_offset, kQ15One, lowband_scratch,
                x_cm));
            y_cm = static_cast<int>(quantBand(
                ctx, by, n, b / 2, b_blocks,
                effective_lowband != -1 ? norm2 + effective_lowband : nullptr, params.lm,
                last ? nullptr : norm2 + m * kCeltEBands[i] - norm_offset, kQ15One, lowband_scratch,
                y_cm));
        } else if (by) {
            x_cm = static_cast<int>(quantBandStereo(
                ctx, bx, by, n, b, b_blocks,
                effective_lowband != -1 ? norm + effective_lowband : nullptr, params.lm,
                last ? nullptr : norm + m * kCeltEBands[i] - norm_offset, lowband_scratch,
                x_cm | y_cm));
            y_cm = x_cm;
        } else {
            x_cm = static_cast<int>(quantBand(
                ctx, bx, n, b, b_blocks,
                effective_lowband != -1 ? norm + effective_lowband : nullptr, params.lm,
                last ? nullptr : norm + m * kCeltEBands[i] - norm_offset, kQ15One, lowband_scratch,
                x_cm | y_cm));
            y_cm = x_cm;
        }

        collapse_masks[i * channels] = static_cast<uint8_t>(x_cm);
        collapse_masks[i * channels + channels - 1] = static_cast<uint8_t>(y_cm);
        balance += params.pulses[i] + tell;
        update_lowband = b > (n << kCeltBitsRes) ? 1 : 0;
    }

    seed = ctx.seed;
}

}  // namespace player::audio::opus

