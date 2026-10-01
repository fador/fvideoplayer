#pragma once

#include <cstdint>

namespace player::audio::silk {

// NLSF codebook (the subset of silk_NLSF_CB_struct needed by the decoder).
struct NlsfCodebook {
    int n_vectors;
    int order;
    int quant_step_size_q16;
    int inv_quant_step_size_q6;
    const uint8_t* cb1_nlsf_q8;
    const int16_t* cb1_wght_q9;
    const uint8_t* cb1_icdf;
    const uint8_t* pred_q8;
    const uint8_t* ec_sel;
    const uint8_t* ec_icdf;
    const uint8_t* ec_rates_q5;
    const int16_t* delta_min_q15;
};

extern const NlsfCodebook kNlsfCodebookNbMb;
extern const NlsfCodebook kNlsfCodebookWb;

}  // namespace player::audio::silk
