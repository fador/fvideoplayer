#pragma once

#include "audio/opus/RangeDecoder.hpp"

#include <cstdint>

namespace player::audio::silk {

// Stereo prediction state (stereo_dec_state).
struct SilkStereoState {
    int32_t pred_prev_q13[2] = {};
    int16_t s_mid[2] = {};
    int16_t s_side[2] = {};
};

// Decodes the mid/side predictors (silk_stereo_decode_pred).
void decodeStereoPred(opus::RangeDecoder& decoder, int32_t* pred_q13);

// Decodes the mid-only flag (silk_stereo_decode_mid_only).
int decodeMidOnly(opus::RangeDecoder& decoder);

// Converts the adaptive mid/side representation to left/right
// (silk_stereo_MS_to_LR). x1 and x2 hold the state in [0], the frame in
// [1, frame_length] and the buffered tail in [frame_length + 1].
void stereoMsToLr(SilkStereoState& state, int16_t* x1, int16_t* x2, const int32_t* pred_q13,
                  int fs_khz, int frame_length);

}  // namespace player::audio::silk
