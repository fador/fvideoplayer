#pragma once

#include <cstdint>

namespace player::audio::silk {

// Dequantizes the gain indices to linear gains (silk_gains_dequant).
// prev_ind is the last gain index of the previous frame (in and out).
void gainsDequant(int32_t* gain_q16, const int8_t* ind, int8_t& prev_ind, int conditional,
                  int nb_subfr);

// Converts a log2 value in Q7 to linear (silk_log2lin).
int32_t log2lin(int32_t in_log_q7);

}  // namespace player::audio::silk
