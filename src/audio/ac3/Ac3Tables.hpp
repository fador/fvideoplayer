#pragma once

#include <cstdint>

namespace player::audio::ac3 {

// Tables from ATSC A/52 (AC-3), shared by the decoder.

// Table 5.18 frame size code table: 16-bit words for 48, 44.1 and 32 kHz.
extern const uint16_t kFrameSize[38][3];
// Table 5.8 audio coding mode to number of full-bandwidth channels.
extern const uint8_t kChannelsTab[8];
extern const int kSampleRates[4];
extern const uint16_t kBitrates[19];

// Section 7.5.2 rematrixing band boundaries.
extern const uint8_t kRematrixBands[5];

// Table 7.18 mapping of bap to quantiser (bits for bap >= 6).
extern const uint8_t kQuantization[16];
// Table 7.17 bit allocation pointer table.
extern const uint8_t kBapTab[64];

// Bit allocation parameters (Tables 7.12 to 7.16).
extern const uint8_t kSlowDecay[4];
extern const uint8_t kFastDecay[4];
extern const uint16_t kSlowGain[4];
extern const uint16_t kDbPerBit[4];
extern const int16_t kFloor[8];
extern const uint16_t kFastGain[8];

// Critical band boundaries and the bin-to-band map.
extern const uint8_t kBandStart[51];
extern const uint8_t kBinToBand[253];

// Bit allocation log-add approximation and the absolute hearing threshold.
extern const uint8_t kLogAdd[260];
extern const uint16_t kHearingThreshold[50][3];

// Section 7.8.2 downmix coefficients, as gain-level indices.
extern const uint8_t kDefaultCoeffs[8][5][2];
// Section 7.7.1 level gains.
extern const float kGainLevels[9];

}  // namespace player::audio::ac3
