#pragma once

#include "audio/opus/RangeDecoder.hpp"

namespace player::audio::opus {

// CELT band energy decoding (RFC 6716 section 4.3). Energies are in the log2
// domain, one value per band and channel, with stride kCeltBands.

// Laplace-distributed integer decoder used by the coarse energies.
int decodeLaplace(RangeDecoder& decoder, unsigned fs, int decay);

// Coarse energy with inter-frame prediction; intra disables the prediction.
void decodeCoarseEnergy(RangeDecoder& decoder, int start, int end, float* old_energy, bool intra,
                        int channels, int lm);

// Refines bands with fine_quant[i] > 0 by that many raw bits.
void decodeFineEnergy(RangeDecoder& decoder, int start, int end, float* old_energy,
                      const int* fine_quant, int channels);

// Spends up to bits_left raw bits on the final energy refinement.
void decodeFinalEnergy(RangeDecoder& decoder, int start, int end, float* old_energy,
                       const int* fine_quant, const int* fine_priority, int bits_left,
                       int channels);

}  // namespace player::audio::opus
