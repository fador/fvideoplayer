#pragma once

#include "audio/aac/AacIcs.hpp"
#include "audio/aac/Mdct.hpp"

namespace player::audio::aac {

// IMDCT, windowing and overlap-add for one channel. Keeps the 1024-sample
// overlap between frames, so one instance is needed per output channel.
class Filterbank {
public:
    void reset();

    // spectrum: 1024 coefficients (8 x 128 for short windows); out: 1024 samples.
    void synthesize(const float* spectrum, const IcsInfo& info, float* out);

private:
    Mdct long_mdct_{1024};
    Mdct short_mdct_{128};
    float overlap_[kFrameSamples] = {};
    float block_[2 * kFrameSamples] = {};
    float window_[2 * kFrameSamples] = {};
    float short_block_[2 * kWindowSize] = {};
    bool previous_shape_ = false;  // window_shape of the previous frame
};

}  // namespace player::audio::aac
