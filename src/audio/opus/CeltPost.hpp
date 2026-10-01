#pragma once

#include <cstdint>

namespace player::audio::opus {

// Post-processing of the decoded bands before the inverse MDCT
// (RFC 6716 section 4.3.5).

// Applies the decoded energies to the unit-norm bands, producing the MDCT
// spectrum. x holds m*eBands[kCeltBands] samples, freq m*kCeltShortMdct.
// silence and downsample come from the frame header.
void denormaliseBands(const float* x, const float* band_log_e, int start, int end, int m,
                      int downsample, int silence, float* freq);

// Replaces the collapsed blocks of transient frames with noise so the energy
// does not drop. x is channels*m*kCeltShortMdct samples; seed is in/out.
void antiCollapse(float* x, const uint8_t* collapse_masks, int lm, int channels, int size,
                  int start, int end, const float* log_e, const float* prev1_log_e,
                  const float* prev2_log_e, const int* pulses, uint32_t& seed);

}  // namespace player::audio::opus
