#pragma once

#include <cstdint>

namespace player::audio::mp3 {

// Tables from the public-domain minimp3 reference (scalar path).
extern const uint8_t kBitrateKbps[2][3][15];
extern const uint32_t kSampleRates[3];

// Scalefactor band widths: long blocks, short blocks and mixed blocks for
// the 8 sample-rate/version combinations (MPEG1 44/48/32, MPEG2 22/24/16,
// MPEG2.5 11/12/8 kHz).
extern const uint8_t kScfLong[8][23];
extern const uint8_t kScfShort[8][40];
extern const uint8_t kScfMixed[8][40];
extern const uint8_t kScfPartitions[3][28];
extern const uint8_t kScfcDecode[16];
extern const uint8_t kMod[24];
extern const uint8_t kPreamp[10];

extern const float kExpFrac[4];
extern const float kPow43[145];

// Huffman codebooks: the compact tree tables and the count1 tables.
extern const int16_t kHuffmanTabs[];
extern const uint8_t kHuffmanTab32[28];
extern const uint8_t kHuffmanTab33[16];
extern const int16_t kHuffmanTabIndex[32];
extern const uint8_t kLinbits[32];

extern const float kPan[14];
extern const float kAliasReduction[2][8];
extern const float kTwid9[18];
extern const float kTwid3[6];
extern const float kMdctWindow[2][18];
extern const float kSec[24];
extern const float kSynthesisWindow[512];

}  // namespace player::audio::mp3
