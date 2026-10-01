#pragma once

#include "audio/opus/CeltVq.hpp"
#include "audio/opus/OpusTables.hpp"
#include "audio/opus/RangeDecoder.hpp"

#include <cstdint>

namespace player::audio::opus {

// Inputs of the band decoding pass (RFC 6716 section 4.3.4), produced by the
// header parsing, energy decoding and bit allocation stages.
struct BandsParams {
    int start = 0;
    int end = kCeltBands;
    int short_blocks = 0;   // 1 for transient frames
    Spread spread = Spread::Normal;
    int dual_stereo = 0;
    int intensity = 0;      // first band coded in intensity stereo
    const int* tf_res = nullptr;   // per-band time/frequency resolution
    const int* pulses = nullptr;   // per-band PVQ budget (1/8 bit units)
    int32_t total_bits = 0;        // packet size in 1/8 bit units
    int32_t balance = 0;           // bits left over by the allocation
    int lm = 0;                    // frame size is 2.5 ms << lm
    int coded_bands = kCeltBands;
    int disable_inv = 0;           // phase inversion off (mono decoders)
};

// Decodes every band into x and y, each M*kCeltEBands[kCeltBands] floats with
// M = 1 << lm. y may be null for mono. The last band's x region doubles as
// scratch space. collapse_masks receives one byte per band and channel; seed
// is the folding random state (in and out).
void quantAllBands(RangeDecoder& decoder, const BandsParams& params, int channels, float* x,
                   float* y, uint8_t* collapse_masks, uint32_t& seed);

}  // namespace player::audio::opus
