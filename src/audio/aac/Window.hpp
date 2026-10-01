#pragma once

#include <cstddef>
#include <vector>

namespace player::audio::aac {

// AAC window shapes. Both produce 2N coefficients for a block of N spectral
// bins and satisfy the Princen-Bradley condition w[i]^2 + w[i+N]^2 = 1 that
// makes the MDCT overlap-add reconstruct the signal.
//
//      sine window (shape 0)          KBD window (shape 1)
//      w[i] = sin(pi*(i+0.5)/2N)      cumulative Kaiser, alpha 4 long / 6 short

std::vector<float> sineWindow(size_t bins);

// alpha: 4 for long blocks (1024 bins), 6 for short blocks (128 bins).
std::vector<float> kbdWindow(size_t bins, float alpha);

}  // namespace player::audio::aac
