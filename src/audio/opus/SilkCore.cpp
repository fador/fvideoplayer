#include "audio/opus/SilkCore.hpp"

#include "audio/opus/SilkLpc.hpp"
#include "audio/opus/SilkMath.hpp"
#include "audio/opus/SilkTables.hpp"

#include <algorithm>
#include <array>

namespace player::audio::silk {
namespace {

constexpr int32_t kQuantLevelAdjustQ10 = 80;
constexpr int32_t kRandMultiplier = 196314165;
constexpr int32_t kRandIncrement = 907633515;

// Wrapping multiply-add pseudo-random generator (silk_RAND).
int32_t randNext(int32_t seed) {
    return static_cast<int32_t>(static_cast<uint32_t>(kRandIncrement) +
                                static_cast<uint32_t>(kRandMultiplier) *
                                    static_cast<uint32_t>(seed));
}

}  // namespace

void decodeCore(SilkCoreState& state, const SilkDecoderControl& control, int16_t* xq,
                const int16_t* pulses) {
    const int ltp_order = kLtpOrder;
    std::vector<int16_t> s_ltp(state.ltp_mem_length);
    std::vector<int32_t> s_ltp_q15(state.ltp_mem_length + state.frame_length);
    std::array<int32_t, kMaxFrameLength> res_q14;
    std::array<int32_t, kMaxFrameLength + kMaxLpcOrder> s_lpc_q14;

    const int32_t offset_q10 =
        kSilkQuantOffsetsQ10[(state.signal_type >> 1) * 2 + state.quant_offset_type];
    const bool nlsf_interpolation = state.nlsf_interp_coef_q2 < 4;

    // Excitation with the quantizer offset and a random sign flip.
    int32_t rand_seed = state.seed;
    for (int i = 0; i < state.frame_length; ++i) {
        rand_seed = randNext(rand_seed);
        int32_t exc = static_cast<int32_t>(pulses[i]) << 14;
        if (exc > 0) {
            exc -= kQuantLevelAdjustQ10 << 4;
        } else if (exc < 0) {
            exc += kQuantLevelAdjustQ10 << 4;
        }
        exc += offset_q10 << 4;
        state.exc_q14[i] = rand_seed < 0 ? -exc : exc;
        rand_seed = static_cast<int32_t>(static_cast<uint32_t>(rand_seed) +
                                         static_cast<uint32_t>(pulses[i]));
    }

    std::copy(state.s_lpc_q14_buf, state.s_lpc_q14_buf + kMaxLpcOrder, s_lpc_q14.begin());

    const int32_t* pexc_q14 = state.exc_q14;
    int16_t* pxq = xq;
    int sltp_buf_idx = state.ltp_mem_length;
    int lag = 0;

    for (int k = 0; k < state.nb_subfr; ++k) {
        const int32_t* pres_q14 = res_q14.data();
        const int16_t* a_q12 = control.pred_coef_q12[k >> 1];
        int signal_type = state.signal_type;
        int subfr_lag = control.pitch_l[k];

        int16_t b_local[kLtpOrder];
        std::copy(&control.ltp_coef_q14[k * ltp_order],
                  &control.ltp_coef_q14[k * ltp_order] + ltp_order, b_local);

        const int32_t gain_q10 = control.gains_q16[k] >> 6;
        int32_t inv_gain_q31 = inverse32VarQ(control.gains_q16[k], 47);

        // Rescale the LPC state when the gain changes.
        int32_t gain_adj_q16 = 1 << 16;
        if (control.gains_q16[k] != state.prev_gain_q16) {
            gain_adj_q16 = div32VarQ(state.prev_gain_q16, control.gains_q16[k], 16);
            for (int i = 0; i < kMaxLpcOrder; ++i) {
                s_lpc_q14[i] = smulww(gain_adj_q16, s_lpc_q14[i]);
            }
        }
        state.prev_gain_q16 = control.gains_q16[k];

        // Smooth the transition from voiced PLC to unvoiced decoding.
        if (state.loss_count != 0 && state.prev_signal_type == kTypeVoiced &&
            state.signal_type != kTypeVoiced && k < kMaxNbSubfr / 2) {
            std::fill(b_local, b_local + ltp_order, static_cast<int16_t>(0));
            b_local[ltp_order / 2] = 4096;  // SILK_FIX_CONST(0.25, 14)
            signal_type = kTypeVoiced;
            subfr_lag = state.lag_prev;
        }
        const int16_t* b_q14 = b_local;

        if (signal_type == kTypeVoiced) {
            lag = subfr_lag;

            // Re-whiten the LTP history with the new LPC coefficients.
            if (k == 0 || (k == 2 && nlsf_interpolation)) {
                const int start_idx = state.ltp_mem_length - lag - state.lpc_order - ltp_order / 2;
                if (k == 2) {
                    std::copy(xq, xq + 2 * state.subfr_length,
                              state.out_buf.begin() + state.ltp_mem_length);
                }
                lpcAnalysisFilter(&s_ltp[start_idx],
                                  &state.out_buf[start_idx + k * state.subfr_length], a_q12,
                                  state.ltp_mem_length - start_idx, state.lpc_order);
                if (k == 0) {
                    // LTP downscaling reduces inter-packet dependency.
                    inv_gain_q31 = smulwb(inv_gain_q31, control.ltp_scale_q14) << 2;
                }
                for (int i = 0; i < lag + ltp_order / 2; ++i) {
                    s_ltp_q15[sltp_buf_idx - i - 1] =
                        smulwb(inv_gain_q31, s_ltp[state.ltp_mem_length - i - 1]);
                }
            } else if (gain_adj_q16 != 1 << 16) {
                for (int i = 0; i < lag + ltp_order / 2; ++i) {
                    s_ltp_q15[sltp_buf_idx - i - 1] =
                        smulww(gain_adj_q16, s_ltp_q15[sltp_buf_idx - i - 1]);
                }
            }
        }

        // Long-term prediction.
        if (signal_type == kTypeVoiced) {
            int32_t* pred_lag_ptr = &s_ltp_q15[sltp_buf_idx - lag + ltp_order / 2];
            for (int i = 0; i < state.subfr_length; ++i) {
                int32_t ltp_pred_q13 = 2;
                ltp_pred_q13 = smlawb(ltp_pred_q13, pred_lag_ptr[0], b_q14[0]);
                ltp_pred_q13 = smlawb(ltp_pred_q13, pred_lag_ptr[-1], b_q14[1]);
                ltp_pred_q13 = smlawb(ltp_pred_q13, pred_lag_ptr[-2], b_q14[2]);
                ltp_pred_q13 = smlawb(ltp_pred_q13, pred_lag_ptr[-3], b_q14[3]);
                ltp_pred_q13 = smlawb(ltp_pred_q13, pred_lag_ptr[-4], b_q14[4]);
                ++pred_lag_ptr;

                res_q14[i] = static_cast<int32_t>(static_cast<uint32_t>(pexc_q14[i]) +
                                                  (static_cast<uint32_t>(ltp_pred_q13) << 1));
                s_ltp_q15[sltp_buf_idx] = res_q14[i] << 1;
                ++sltp_buf_idx;
            }
        } else {
            pres_q14 = pexc_q14;
        }

        // Short-term prediction and gain scaling.
        for (int i = 0; i < state.subfr_length; ++i) {
            int32_t lpc_pred_q10 = state.lpc_order >> 1;
            for (int j = 0; j < state.lpc_order; ++j) {
                lpc_pred_q10 =
                    smlawb(lpc_pred_q10, s_lpc_q14[kMaxLpcOrder + i - 1 - j], a_q12[j]);
            }
            s_lpc_q14[kMaxLpcOrder + i] =
                addSat32(pres_q14[i], lshiftSat32(lpc_pred_q10, 4));
            pxq[i] = static_cast<int16_t>(sat16(
                rshiftRound(smulww(s_lpc_q14[kMaxLpcOrder + i], gain_q10), 8)));
        }

        // Update the LPC filter state.
        std::copy(s_lpc_q14.begin() + state.subfr_length,
                  s_lpc_q14.begin() + state.subfr_length + kMaxLpcOrder, s_lpc_q14.begin());
        pexc_q14 += state.subfr_length;
        pxq += state.subfr_length;
    }

    std::copy(s_lpc_q14.begin(), s_lpc_q14.begin() + kMaxLpcOrder, state.s_lpc_q14_buf);
}

}  // namespace player::audio::silk



