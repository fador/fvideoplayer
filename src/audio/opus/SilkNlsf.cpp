#include "audio/opus/SilkNlsf.hpp"

#include "audio/opus/SilkMath.hpp"

namespace player::audio::silk {
namespace {

constexpr int kMaxLpcOrder = 16;
constexpr int kQuantMaxAmplitude = 4;  // NLSF_QUANT_MAX_AMPLITUDE
constexpr int32_t kQuantLevelAdjQ10 = 102;  // SILK_FIX_CONST(0.1, 10)
constexpr int kMaxLoops = 20;

// silk_NLSF_unpack: entropy table indices and predictors per coefficient.
void unpack(int16_t* ec_ix, uint8_t* pred_q8, const NlsfCodebook& cb, int cb1_index) {
    const uint8_t* ec_sel = cb.ec_sel + cb1_index * cb.order / 2;
    for (int i = 0; i < cb.order; i += 2) {
        const uint8_t entry = *ec_sel++;
        ec_ix[i] = static_cast<int16_t>(smulbb((entry >> 1) & 7, 2 * kQuantMaxAmplitude + 1));
        pred_q8[i] = cb.pred_q8[i + (entry & 1) * (cb.order - 1)];
        ec_ix[i + 1] = static_cast<int16_t>(smulbb((entry >> 5) & 7, 2 * kQuantMaxAmplitude + 1));
        pred_q8[i + 1] = cb.pred_q8[i + ((entry >> 4) & 1) * (cb.order - 1) + 1];
    }
}

// silk_NLSF_residual_dequant: backward-predictive dequantizer.
void residualDequant(int16_t* x_q10, const int8_t* indices, const uint8_t* pred_coef_q8,
                     int quant_step_size_q16, int order) {
    int32_t out_q10 = 0;
    for (int i = order - 1; i >= 0; --i) {
        const int32_t pred_q10 = smulbb(out_q10, pred_coef_q8[i]) >> 8;
        out_q10 = static_cast<int32_t>(indices[i]) << 10;
        if (out_q10 > 0) {
            out_q10 -= kQuantLevelAdjQ10;
        } else if (out_q10 < 0) {
            out_q10 += kQuantLevelAdjQ10;
        }
        out_q10 = smlawb(pred_q10, out_q10, quant_step_size_q16);
        x_q10[i] = static_cast<int16_t>(out_q10);
    }
}

// silk_NLSF_stabilize: pushes the NLSFs apart and away from the borders.
void stabilize(int16_t* nlsf_q15, const int16_t* delta_min_q15, int order) {
    for (int loops = 0; loops < kMaxLoops; ++loops) {
        // Find the smallest distance.
        int32_t min_diff = nlsf_q15[0] - delta_min_q15[0];
        int index = 0;
        for (int i = 1; i <= order - 1; ++i) {
            const int32_t diff = nlsf_q15[i] - (nlsf_q15[i - 1] + delta_min_q15[i]);
            if (diff < min_diff) {
                min_diff = diff;
                index = i;
            }
        }
        const int32_t diff = (1 << 15) - (nlsf_q15[order - 1] + delta_min_q15[order]);
        if (diff < min_diff) {
            min_diff = diff;
            index = order;
        }

        if (min_diff >= 0) {
            return;
        }

        if (index == 0) {
            nlsf_q15[0] = delta_min_q15[0];
        } else if (index == order) {
            nlsf_q15[order - 1] = static_cast<int16_t>((1 << 15) - delta_min_q15[order]);
        } else {
            // Keep the center frequency, move the two neighbors apart.
            int32_t min_center = 0;
            for (int k = 0; k < index; ++k) {
                min_center += delta_min_q15[k];
            }
            min_center += delta_min_q15[index] >> 1;

            int32_t max_center = 1 << 15;
            for (int k = order; k > index; --k) {
                max_center -= delta_min_q15[k];
            }
            max_center -= delta_min_q15[index] >> 1;

            const int16_t center = static_cast<int16_t>(limit(
                rshiftRound(static_cast<int32_t>(nlsf_q15[index - 1]) + nlsf_q15[index], 1),
                min_center, max_center));
            nlsf_q15[index - 1] = static_cast<int16_t>(center - (delta_min_q15[index] >> 1));
            nlsf_q15[index] = static_cast<int16_t>(nlsf_q15[index - 1] + delta_min_q15[index]);
        }
    }

    // Fallback: sort and enforce the minimum distances.
    insertionSortIncreasing(nlsf_q15, order);
    nlsf_q15[0] = static_cast<int16_t>(nlsf_q15[0] > delta_min_q15[0] ? nlsf_q15[0]
                                                                      : delta_min_q15[0]);
    for (int i = 1; i < order; ++i) {
        const int16_t min_value = static_cast<int16_t>(addSat16(nlsf_q15[i - 1], delta_min_q15[i]));
        nlsf_q15[i] = static_cast<int16_t>(nlsf_q15[i] > min_value ? nlsf_q15[i] : min_value);
    }
    nlsf_q15[order - 1] = static_cast<int16_t>(
        nlsf_q15[order - 1] < (1 << 15) - delta_min_q15[order] ? nlsf_q15[order - 1]
                                                               : (1 << 15) - delta_min_q15[order]);
    for (int i = order - 2; i >= 0; --i) {
        const int16_t max_value = static_cast<int16_t>(nlsf_q15[i + 1] - delta_min_q15[i + 1]);
        nlsf_q15[i] = static_cast<int16_t>(nlsf_q15[i] < max_value ? nlsf_q15[i] : max_value);
    }
}

}  // namespace

void nlsfDecode(int16_t* nlsf_q15, const int8_t* indices, const NlsfCodebook& codebook) {
    uint8_t pred_q8[kMaxLpcOrder];
    int16_t ec_ix[kMaxLpcOrder];
    int16_t res_q10[kMaxLpcOrder];

    unpack(ec_ix, pred_q8, codebook, indices[0]);
    residualDequant(res_q10, indices + 1, pred_q8, codebook.quant_step_size_q16, codebook.order);

    const uint8_t* cb_element = codebook.cb1_nlsf_q8 + indices[0] * codebook.order;
    const int16_t* cb_wght = codebook.cb1_wght_q9 + indices[0] * codebook.order;
    for (int i = 0; i < codebook.order; ++i) {
        const int32_t tmp = (static_cast<int32_t>(res_q10[i]) << 14) / cb_wght[i] +
                            (static_cast<int16_t>(cb_element[i]) << 7);
        nlsf_q15[i] = static_cast<int16_t>(limit(tmp, 0, 32767));
    }

    stabilize(nlsf_q15, codebook.delta_min_q15, codebook.order);
}

}  // namespace player::audio::silk
