#pragma once

#include "audio/opus/SilkParameters.hpp"

#include <cstdint>
#include <vector>

namespace player::audio::silk {

constexpr int kMaxFrameLength = 320;  // 20 ms at 16 kHz

// State used by the core decoder (silk_decoder_state subset).
struct SilkCoreState {
    int lpc_order = 0;
    int nb_subfr = 0;
    int subfr_length = 0;
    int frame_length = 0;
    int ltp_mem_length = 0;
    int signal_type = 0;
    int quant_offset_type = 0;
    int nlsf_interp_coef_q2 = 0;
    int seed = 0;
    int loss_count = 0;
    int prev_signal_type = 0;
    int lag_prev = 0;
    int32_t prev_gain_q16 = 0;
    int32_t exc_q14[kMaxFrameLength] = {};
    int32_t s_lpc_q14_buf[kMaxLpcOrder] = {};
    std::vector<int16_t> out_buf;  // decoded history: ltp_mem_length + frame_length
};

// Inverse noise shaping: LTP and LPC synthesis (silk_decode_core). xq
// receives frame_length samples; pulses holds the excitation.
void decodeCore(SilkCoreState& state, const SilkDecoderControl& control, int16_t* xq,
                const int16_t* pulses);

}  // namespace player::audio::silk
