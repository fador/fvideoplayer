#pragma once

#include "audio/opus/OpusTables.hpp"
#include "audio/opus/RangeDecoder.hpp"

#include <cstdint>

namespace player::audio::opus {

// Result of the CELT bit allocation (RFC 6716 section 4.3.3).
struct Allocation {
    int coded_bands = 0;
    int intensity = 0;
    int dual_stereo = 0;
    int32_t balance = 0;
    int pulses[kCeltBands] = {};        // PVQ bits per band (1/8 bit resolution)
    int fine_quant[kCeltBands] = {};    // fine energy bits per band
    int fine_priority[kCeltBands] = {}; // priority of the final energy pass
};

// Per-band bit caps for the given frame size and channel count.
void initCaps(int* cap, int lm, int channels);

// Decodes dynalloc offsets, skip/intensity/dual decisions and the per-band
// bit allocation from the range decoder.
void computeAllocation(RangeDecoder& decoder, int start, int end, const int* offsets,
                       const int* cap, int alloc_trim, int32_t total, int channels, int lm,
                       Allocation& out);

}  // namespace player::audio::opus
