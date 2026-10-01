#pragma once

#include "audio/opus/SilkIndices.hpp"
#include "audio/opus/SilkTables.hpp"

#include <cstdint>

namespace player::audio::silk {

constexpr int kLtpOrder = 5;

// Per-frame decoder control parameters (silk_decoder_control subset).
struct SilkDecoderControl {
    int32_t gains_q16[kMaxNbSubfr] = {};
    int16_t pred_coef_q12[2][kMaxLpcOrder] = {};
    int16_t ltp_coef_q14[kMaxNbSubfr * kLtpOrder] = {};
    int pitch_l[kMaxNbSubfr] = {};
    int32_t ltp_scale_q14 = 0;
};

// State used while assembling the control parameters.
struct SilkParamState {
    int lpc_order = 0;
    int nb_subfr = 0;
    int fs_khz = 0;
    const NlsfCodebook* nlsf_codebook = nullptr;
    int16_t prev_nlsf_q15[kMaxLpcOrder] = {};
    int8_t last_gain_index = 10;
    int first_frame_after_reset = 0;
    int loss_count = 0;
};

// Dequantizes gains, converts the NLSFs to LPC coefficients with
// interpolation, and expands the LTP parameters (silk_decode_parameters).
void decodeParameters(SilkParamState& state, SilkIndices& indices, CondCoding cond_coding,
                      SilkDecoderControl& control);

}  // namespace player::audio::silk
