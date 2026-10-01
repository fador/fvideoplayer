#pragma once

#include "audio/opus/SilkTables.hpp"

#include <cstdint>

namespace player::audio::silk {

// Entropy table indices and predictors for a first-stage codebook index
// (silk_NLSF_unpack).
void nlsfUnpack(int16_t* ec_ix, uint8_t* pred_q8, const NlsfCodebook& codebook, int cb1_index);

// Decodes an NLSF vector from its codebook path indices
// (silk_NLSF_decode from libopus). indices[0] selects the first-stage
// vector, the rest are residuals in [-4, 4]. Output is the stabilized
// vector in Q15.
void nlsfDecode(int16_t* nlsf_q15, const int8_t* indices, const NlsfCodebook& codebook);

}  // namespace player::audio::silk
