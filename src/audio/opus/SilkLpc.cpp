#include "audio/opus/SilkLpc.hpp"

#include "audio/opus/SilkMath.hpp"
#include "audio/opus/SilkTables.hpp"

#include <algorithm>

namespace player::audio::silk {
namespace {

constexpr int kQa = 16;  // NLSF2A polynomial domain
constexpr int kLpcQa = 24;  // LPC_inverse_pred_gain domain
constexpr int32_t kALimit = 16773022;  // SILK_FIX_CONST(0.99975, 24)
constexpr int32_t kMinGainQ30 = 107374;  // SILK_FIX_CONST(1/1e4, 30)
constexpr int32_t kChirp0999Q16 = 65470;  // SILK_FIX_CONST(0.999, 16)
constexpr int kMaxStabilizeIterations = 16;

// silk_NLSF2A_find_poly: one of the two polynomials of the LSF pair.
void findPoly(int32_t* out, const int32_t* c_lsf, int dd) {
    out[0] = 1 << kQa;
    out[1] = -c_lsf[0];
    for (int k = 1; k < dd; ++k) {
        const int32_t ftmp = c_lsf[2 * k];
        out[k + 1] = (out[k - 1] << 1) - static_cast<int32_t>(rshiftRound64(smull(ftmp, out[k]), kQa));
        for (int n = k; n > 1; --n) {
            out[n] += out[n - 2] -
                      static_cast<int32_t>(rshiftRound64(smull(ftmp, out[n - 1]), kQa));
        }
        out[1] -= ftmp;
    }
}

// Variable-Q reciprocal (silk_INVERSE32_varQ from Inlines.h).
int32_t mul32FracQ(int32_t a, int32_t b, int q) {
    return static_cast<int32_t>(rshiftRound64(smull(a, b), q));
}

}  // namespace

int32_t inverse32VarQ(int32_t b32, int qres) {
    const int b_headrm = clz32(std::abs(b32)) - 1;
    const int32_t b32_nrm = b32 << b_headrm;
    const int32_t b32_inv = (kInt32Max >> 2) / (b32_nrm >> 16);

    int32_t result = b32_inv << 16;
    const int32_t err_q32 = ((1 << 29) - smulwb(b32_nrm, b32_inv)) << 3;
    result = smlaww(result, err_q32, b32_inv);

    const int lshift = 61 - b_headrm - qres;
    if (lshift <= 0) {
        return lshiftSat32(result, -lshift);
    }
    if (lshift < 32) {
        return result >> lshift;
    }
    return 0;
}

int32_t div32VarQ(int32_t a32, int32_t b32, int qres) {
    const int a_headrm = clz32(std::abs(a32)) - 1;
    int32_t a32_nrm = a32 << a_headrm;
    const int b_headrm = clz32(std::abs(b32)) - 1;
    const int32_t b32_nrm = b32 << b_headrm;
    const int32_t b32_inv = (kInt32Max >> 2) / (b32_nrm >> 16);

    int32_t result = smulwb(a32_nrm, b32_inv);
    a32_nrm = static_cast<int32_t>(static_cast<uint32_t>(a32_nrm) -
                                   (static_cast<uint32_t>(smmul(b32_nrm, result)) << 3));
    result = smlawb(result, a32_nrm, b32_inv);

    const int lshift = 29 + a_headrm - b_headrm - qres;
    if (lshift < 0) {
        return lshiftSat32(result, -lshift);
    }
    if (lshift < 32) {
        return result >> lshift;
    }
    return 0;
}

void lpcAnalysisFilter(int16_t* out, const int16_t* in, const int16_t* b, int len, int d) {
    for (int ix = d; ix < len; ++ix) {
        const int16_t* in_ptr = &in[ix - 1];

        int32_t out32_q12 = smulbb(in_ptr[0], b[0]);
        out32_q12 = smlabbOvflw(out32_q12, in_ptr[-1], b[1]);
        out32_q12 = smlabbOvflw(out32_q12, in_ptr[-2], b[2]);
        out32_q12 = smlabbOvflw(out32_q12, in_ptr[-3], b[3]);
        out32_q12 = smlabbOvflw(out32_q12, in_ptr[-4], b[4]);
        out32_q12 = smlabbOvflw(out32_q12, in_ptr[-5], b[5]);
        for (int j = 6; j < d; j += 2) {
            out32_q12 = smlabbOvflw(out32_q12, in_ptr[-j], b[j]);
            out32_q12 = smlabbOvflw(out32_q12, in_ptr[-j - 1], b[j + 1]);
        }

        // Subtract the prediction and scale to Q0.
        out32_q12 = static_cast<int32_t>(static_cast<uint32_t>(in_ptr[1]) << 12) -
                    static_cast<uint32_t>(out32_q12);
        out[ix] = static_cast<int16_t>(sat16(rshiftRound(out32_q12, 12)));
    }
    for (int j = 0; j < d; ++j) {
        out[j] = 0;
    }
}

void lpcFit(int16_t* a_qout, int32_t* a_qin, int qout, int qin, int d) {
    int i = 0;
    for (; i < 10; ++i) {
        // Find the largest coefficient; expand the bandwidth when it does
        // not fit in int16.
        int32_t maxabs = 0;
        int idx = 0;
        for (int k = 0; k < d; ++k) {
            const int32_t absval = std::abs(a_qin[k]);
            if (absval > maxabs) {
                maxabs = absval;
                idx = k;
            }
        }
        maxabs = rshiftRound(maxabs, qin - qout);
        if (maxabs <= kInt16Max) {
            break;
        }
        maxabs = std::min(maxabs, 163838);
        const int32_t chirp_q16 =
            kChirp0999Q16 -
            static_cast<int32_t>((static_cast<int64_t>(maxabs - kInt16Max) << 14) /
                                 (static_cast<int32_t>(smull(maxabs, idx + 1)) >> 2));
        bwexpander32(a_qin, d, chirp_q16);
    }

    if (i == 10) {
        for (int k = 0; k < d; ++k) {
            a_qout[k] = static_cast<int16_t>(sat16(rshiftRound(a_qin[k], qin - qout)));
            a_qin[k] = static_cast<int32_t>(a_qout[k]) << (qin - qout);
        }
        return;
    }
    for (int k = 0; k < d; ++k) {
        a_qout[k] = static_cast<int16_t>(rshiftRound(a_qin[k], qin - qout));
    }
}

void bwexpander32(int32_t* ar, int d, int32_t chirp_q16) {
    const int32_t chirp_minus_one_q16 = chirp_q16 - 65536;
    for (int i = 0; i < d - 1; ++i) {
        ar[i] = smulww(chirp_q16, ar[i]);
        chirp_q16 += rshiftRound(mul32(chirp_q16, chirp_minus_one_q16), 16);
    }
    ar[d - 1] = smulww(chirp_q16, ar[d - 1]);
}

void bwexpander(int16_t* ar, int d, int32_t chirp_q16) {
    const int32_t chirp_minus_one_q16 = chirp_q16 - 65536;
    for (int i = 0; i < d - 1; ++i) {
        ar[i] = static_cast<int16_t>(rshiftRound(mul32(chirp_q16, ar[i]), 16));
        chirp_q16 += rshiftRound(mul32(chirp_q16, chirp_minus_one_q16), 16);
    }
    ar[d - 1] = static_cast<int16_t>(rshiftRound(mul32(chirp_q16, ar[d - 1]), 16));
}

void interpolate(int16_t* xi, const int16_t* x0, const int16_t* x1, int ifact_q2, int d) {
    for (int i = 0; i < d; ++i) {
        xi[i] = static_cast<int16_t>(x0[i] + (smulbb(x1[i] - x0[i], ifact_q2) >> 2));
    }
}

int32_t lpcInversePredGain(const int16_t* a_q12, int order) {
    int32_t a_qa[16];
    int32_t dc_resp = 0;
    for (int k = 0; k < order; ++k) {
        dc_resp += a_q12[k];
        a_qa[k] = static_cast<int32_t>(a_q12[k]) << (kLpcQa - 12);
    }
    if (dc_resp >= 4096) {
        return 0;
    }

    int32_t inv_gain_q30 = 1 << 30;
    int k = order - 1;
    for (; k > 0; --k) {
        if (a_qa[k] > kALimit || a_qa[k] < -kALimit) {
            return 0;
        }
        const int32_t rc_q31 = -(a_qa[k] << (31 - kLpcQa));
        const int32_t rc_mult1_q30 = (1 << 30) - smmul(rc_q31, rc_q31);
        inv_gain_q30 = smmul(inv_gain_q30, rc_mult1_q30) << 2;
        if (inv_gain_q30 < kMinGainQ30) {
            return 0;
        }

        const int mult2q = 32 - clz32(std::abs(rc_mult1_q30));
        const int32_t rc_mult2 = inverse32VarQ(rc_mult1_q30, mult2q + 30);
        for (int n = 0; n < (k + 1) >> 1; ++n) {
            const int32_t tmp1 = a_qa[n];
            const int32_t tmp2 = a_qa[k - n - 1];
            int64_t tmp64 =
                rshiftRound64(smull(subSat32(tmp1, mul32FracQ(tmp2, rc_q31, 31)), rc_mult2), mult2q);
            if (tmp64 > kInt32Max || tmp64 < kInt32Min) {
                return 0;
            }
            a_qa[n] = static_cast<int32_t>(tmp64);
            tmp64 =
                rshiftRound64(smull(subSat32(tmp2, mul32FracQ(tmp1, rc_q31, 31)), rc_mult2), mult2q);
            if (tmp64 > kInt32Max || tmp64 < kInt32Min) {
                return 0;
            }
            a_qa[k - n - 1] = static_cast<int32_t>(tmp64);
        }
    }

    if (a_qa[k] > kALimit || a_qa[k] < -kALimit) {
        return 0;
    }
    const int32_t rc_q31 = -(a_qa[0] << (31 - kLpcQa));
    const int32_t rc_mult1_q30 = (1 << 30) - smmul(rc_q31, rc_q31);
    inv_gain_q30 = smmul(inv_gain_q30, rc_mult1_q30) << 2;
    if (inv_gain_q30 < kMinGainQ30) {
        return 0;
    }
    return inv_gain_q30;
}

void nlsfToLpc(int16_t* a_q12, const int16_t* nlsf, int order) {
    // Interleaved ordering maximizes numerical accuracy.
    static const uint8_t kOrdering16[16] = {0, 15, 8, 7, 4, 11, 12, 3, 2, 13, 10, 5, 6, 9, 14, 1};
    static const uint8_t kOrdering10[10] = {0, 9, 6, 3, 4, 5, 8, 1, 2, 7};
    const uint8_t* ordering = order == 16 ? kOrdering16 : kOrdering10;

    // NLSFs to 2*cos(LSF) with the piecewise linear table.
    int32_t cos_lsf_qa[16];
    for (int k = 0; k < order; ++k) {
        const int32_t f_int = nlsf[k] >> 8;
        const int32_t f_frac = nlsf[k] - (f_int << 8);
        const int32_t cos_val = kLsfCosTableQ12[f_int];
        const int32_t delta = kLsfCosTableQ12[f_int + 1] - cos_val;
        cos_lsf_qa[ordering[k]] =
            rshiftRound((cos_val << 8) + delta * f_frac, 20 - kQa);
    }

    const int dd = order >> 1;
    int32_t p[9];
    int32_t q[9];
    findPoly(p, &cos_lsf_qa[0], dd);
    findPoly(q, &cos_lsf_qa[1], dd);

    // Even and odd polynomials to Q12 filter coefficients.
    int32_t a32_qa1[16];
    for (int k = 0; k < dd; ++k) {
        const int32_t ptmp = p[k + 1] + p[k];
        const int32_t qtmp = q[k + 1] - q[k];
        a32_qa1[k] = -qtmp - ptmp;
        a32_qa1[order - k - 1] = qtmp - ptmp;
    }
    lpcFit(a_q12, a32_qa1, 12, kQa + 1, order);

    // Bandwidth expand until the filter is stable.
    for (int i = 0; lpcInversePredGain(a_q12, order) == 0 && i < kMaxStabilizeIterations; ++i) {
        bwexpander32(a32_qa1, order, 65536 - (2 << i));
        for (int k = 0; k < order; ++k) {
            a_q12[k] = static_cast<int16_t>(rshiftRound(a32_qa1[k], kQa + 1 - 12));
        }
    }
}

}  // namespace player::audio::silk
