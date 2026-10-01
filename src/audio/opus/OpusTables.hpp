#pragma once

#include <cstdint>

namespace player::audio::opus {

// CELT constants for the 48 kHz mode used by standard Opus streams.
constexpr int kCeltBands = 21;         // number of energy bands
constexpr int kCeltOverlap = 120;      // MDCT overlap in samples
constexpr int kCeltShortMdct = 120;    // shortest MDCT size (2.5 ms)
constexpr int kCeltMaxLm = 3;          // 2.5 ms << 3 = 20 ms
constexpr int kCeltFrame48k = 960;     // 20 ms at 48 kHz
constexpr int kCeltMaxFineBits = 8;
constexpr int kCeltBitsRes = 3;        // 1/8 bit resolution

// Energy band edges in MDCT bins for a 2.5 ms frame (kCeltBands + 1 entries).
extern const int16_t kCeltEBands[kCeltBands + 1];

// Bit allocation table indexed by [bitrate][band] (11 x 21).
extern const uint8_t kCeltBandAllocation[11 * kCeltBands];

// Coarse energy probability model [LM][intra][2*band + side] (4 x 2 x 42).
extern const uint8_t kCeltEnergyProb[4 * 2 * 42];

// MDCT overlap window (Vorbis window), kCeltOverlap entries.
extern const float kCeltWindow[kCeltOverlap];

// log2 of the number of MDCT bins per energy band (Q? plain int).
extern const int16_t kCeltLogN[kCeltBands];

// Per-band bit caps indexed by [2*LM + channels - 1][band] (8 x 21).
extern const uint8_t kCeltCacheCaps[168];

// Inverse CDFs used by the range coder.
extern const uint8_t kCeltTrimIcdf[11];
extern const uint8_t kCeltSpreadIcdf[4];
extern const uint8_t kCeltTapsetIcdf[3];
extern const uint8_t kCeltSmallEnergyIcdf[3];

// log2 in 1/8 bit steps for allocation reservations (24 entries).
extern const uint8_t kCeltLog2Frac[24];

// Bit-reversal ordering used by anti-collapse (30 entries).
extern const int16_t kCeltOrdery[30];

}  // namespace player::audio::opus
