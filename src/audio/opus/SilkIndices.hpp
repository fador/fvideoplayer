#pragma once

#include "audio/opus/RangeDecoder.hpp"
#include "audio/opus/SilkTables.hpp"

#include <cstdint>

namespace player::audio::silk {

constexpr int kMaxNbSubfr = 4;
constexpr int kMaxLpcOrder = 16;

// Frame coding types (define.h).
constexpr int kTypeNoVoiceActivity = 0;
constexpr int kTypeUnvoiced = 1;
constexpr int kTypeVoiced = 2;

enum class CondCoding {
    Independently = 0,
    IndependentlyNoLtpScaling = 1,
    Conditionally = 2,
};

enum class DecodeLbrr {
    No,
    Yes,
};

// Side information decoded from the frame payload (SideInfoIndices).
struct SilkIndices {
    int8_t signal_type = 0;
    int8_t quant_offset_type = 0;
    int8_t gains_indices[kMaxNbSubfr] = {};
    int8_t nlsf_indices[kMaxLpcOrder + 1] = {};
    int8_t nlsf_interp_coef_q2 = 0;
    int16_t lag_index = 0;
    int8_t contour_index = 0;
    int8_t per_index = 0;
    int8_t ltp_index[kMaxNbSubfr] = {};
    int8_t ltp_scale_index = 0;
    int8_t seed = 0;
};

// Decoder state fields the side information decoding depends on.
struct SilkState {
    int fs_khz = 16;
    int nb_subfr = kMaxNbSubfr;
    const NlsfCodebook* nlsf_codebook = nullptr;
    const uint8_t* pitch_lag_low_bits_icdf = nullptr;
    const uint8_t* pitch_contour_icdf = nullptr;
    const uint8_t* vad_flags = nullptr;  // one flag per frame in the packet
    int ec_prev_signal_type = kTypeNoVoiceActivity;
    int ec_prev_lag_index = 0;
};

// Decodes the side information of one SILK frame (silk_decode_indices).
void decodeIndices(SilkState& state, opus::RangeDecoder& decoder, int frame_index, DecodeLbrr decode_lbrr,
                   CondCoding cond_coding, SilkIndices& indices);

}  // namespace player::audio::silk
