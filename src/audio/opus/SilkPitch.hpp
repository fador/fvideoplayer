#pragma once

#include <cstdint>

namespace player::audio::silk {

// Expands the pitch lag index and contour into per-subframe lags
// (silk_decode_pitch). pitch_lags receives nb_subfr values.
void decodePitch(int16_t lag_index, int8_t contour_index, int* pitch_lags, int fs_khz,
                 int nb_subfr);

}  // namespace player::audio::silk
