#include "audio/opus/SilkParameters.hpp"

#include "audio/opus/SilkGain.hpp"
#include "audio/opus/SilkLpc.hpp"
#include "audio/opus/SilkNlsf.hpp"
#include "audio/opus/SilkPitch.hpp"

#include <algorithm>

namespace player::audio::silk {
namespace {

constexpr int32_t kBweAfterLossQ16 = 63570;

}  // namespace

void decodeParameters(SilkParamState& state, SilkIndices& indices, CondCoding cond_coding,
                      SilkDecoderControl& control) {
    gainsDequant(control.gains_q16, indices.gains_indices, state.last_gain_index,
                 cond_coding == CondCoding::Conditionally ? 1 : 0, state.nb_subfr);

    // Current NLSFs to the second LPC set.
    int16_t nlsf_q15[kMaxLpcOrder];
    nlsfDecode(nlsf_q15, indices.nlsf_indices, *state.nlsf_codebook);
    nlsfToLpc(control.pred_coef_q12[1], nlsf_q15, state.lpc_order);

    // After a reset interpolation is not allowed (helps after a switch).
    if (state.first_frame_after_reset == 1) {
        indices.nlsf_interp_coef_q2 = 4;
    }

    if (indices.nlsf_interp_coef_q2 < 4) {
        // Interpolated NLSFs between the previous and current frame.
        int16_t nlsf0_q15[kMaxLpcOrder];
        for (int i = 0; i < state.lpc_order; ++i) {
            nlsf0_q15[i] = static_cast<int16_t>(
                state.prev_nlsf_q15[i] +
                ((indices.nlsf_interp_coef_q2 * (nlsf_q15[i] - state.prev_nlsf_q15[i])) >> 2));
        }
        nlsfToLpc(control.pred_coef_q12[0], nlsf0_q15, state.lpc_order);
    } else {
        std::copy(control.pred_coef_q12[1], control.pred_coef_q12[1] + state.lpc_order,
                  control.pred_coef_q12[0]);
    }
    std::copy(nlsf_q15, nlsf_q15 + state.lpc_order, state.prev_nlsf_q15);

    // Bandwidth expansion after a packet loss.
    if (state.loss_count != 0) {
        bwexpander(control.pred_coef_q12[0], state.lpc_order, kBweAfterLossQ16);
        bwexpander(control.pred_coef_q12[1], state.lpc_order, kBweAfterLossQ16);
    }

    if (indices.signal_type == kTypeVoiced) {
        decodePitch(indices.lag_index, indices.contour_index, control.pitch_l, state.fs_khz,
                    state.nb_subfr);

        // LTP filter coefficients from the periodicity codebook.
        const int8_t* cbk = kSilkLtpGainVq0;
        if (indices.per_index == 1) {
            cbk = kSilkLtpGainVq1;
        } else if (indices.per_index == 2) {
            cbk = kSilkLtpGainVq2;
        }
        for (int k = 0; k < state.nb_subfr; ++k) {
            const int ix = indices.ltp_index[k];
            for (int i = 0; i < kLtpOrder; ++i) {
                control.ltp_coef_q14[k * kLtpOrder + i] =
                    static_cast<int16_t>(cbk[ix * kLtpOrder + i] << 7);
            }
        }
        control.ltp_scale_q14 = kSilkLtpScalesQ14[indices.ltp_scale_index];
    } else {
        std::fill(control.pitch_l, control.pitch_l + state.nb_subfr, 0);
        std::fill(control.ltp_coef_q14, control.ltp_coef_q14 + kLtpOrder * state.nb_subfr,
                  static_cast<int16_t>(0));
        indices.per_index = 0;
        control.ltp_scale_q14 = 0;
    }
}

}  // namespace player::audio::silk
