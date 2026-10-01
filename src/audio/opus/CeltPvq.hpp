#pragma once

#include "audio/opus/RangeDecoder.hpp"

#include <cstdint>

namespace player::audio::opus {

// PVQ (pyramid vector quantization) decoding from RFC 6716 section 4.3.4.
//
// A codeword encodes a vector of n integer samples whose absolute values sum
// to k, plus one sign bit per non-zero sample. V(n, k) is the number of
// codewords.

// Number of PVQ codewords V(n, k). Requires n >= 2 and k > 0.
uint32_t pvqCount(int n, int k);

// Converts a codeword index into its pulse vector and returns the sum of
// squares of the vector (used for normalization).
float indexToPulses(int n, int k, uint32_t index, int* out);

// Reads one PVQ codeword from the range decoder.
float decodePulses(RangeDecoder& decoder, int n, int k, int* out);

}  // namespace player::audio::opus
