#pragma once

#include "audio/opus/SilkTables.hpp"

#include <cstdint>

namespace player::audio::silk {

// Decodes an NLSF vector from its codebook path indices
// (silk_NLSF_decode from libopus). indices[0] selects the first-stage
// vector, the rest are residuals in [-4, 4]. Output is the stabilized
// vector in Q15.
void nlsfDecode(int16_t* nlsf_q15, const int8_t* indices, const NlsfCodebook& codebook);

}  // namespace player::audio::silk
