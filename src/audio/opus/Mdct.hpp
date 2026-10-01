#pragma once

#include "audio/opus/KissFft.hpp"
#include "audio/opus/OpusTables.hpp"

#include <array>
#include <vector>

namespace player::audio::opus {

// CELT inverse MDCT with integrated windowing (libopus mdct_lookup).
class MdctLookup {
public:
    void init(int n, int max_shift);

    // Inverse MDCT of the stride-interleaved bins in, writing
    // overlap/2 + (n >> shift)/2 samples to out. window is the CELT overlap
    // window.
    void backward(const float* in, float* out, const float* window, int overlap, int shift,
                  int stride) const;

private:
    int n_ = 0;
    int max_shift_ = 0;
    std::array<FftState, kCeltMaxLm + 1> fft_;
    std::vector<float> trig_;
};

}  // namespace player::audio::opus
