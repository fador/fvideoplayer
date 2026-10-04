#pragma once

#include "decode/mpeg12/Mpeg12Tables.hpp"

#include <cstdint>

namespace player::decode::flv1 {

using mpeg12::VlcEntry;

// Sorenson Spark (FLV1) is H.263 with a 17-bit picture start code, its own
// picture header and a different escape form for the run-level codes. The
// variable-length code tables below are the H.263 baseline tables, transcribed
// from ISO/IEC 11578 Table 14 and the MCBPC / CBPY / MV tables.

// intra_mcbpc: index is the macroblock type, 0..7 plus the stuffing code 8.
extern const VlcEntry kIntraMcbpc[9];
// inter_mcbpc: 28 entries, of which the four codes with zero length do not
// exist; stuffing maps to index 20 and is skipped by the caller.
extern const VlcEntry kInterMcbpc[25];
// cbpy: index is the 4-bit luma coded block pattern.
extern const VlcEntry kCbpy[16];
// Motion vector difference codes; the symbol is the code number d.
extern const VlcEntry kMotionVlc[33];
// TCOEF: 102 run-level codes plus the escape at index 102. `value` is the
// table index; kRunLevelRun/Level carry the run and magnitude.
extern const VlcEntry kRunLevel[103];
extern const int8_t kRunLevelRun[102];
extern const int8_t kRunLevelLevel[102];
constexpr int kRunLevelEscape = 102;
// Entries from this index on end the block (the H.263 RL table's "last").
constexpr int kRunLevelLast = 58;

// H.263 half-pel chroma vector rounding for 4MV macroblocks.
extern const uint8_t kChromaRoundTable[16];

}  // namespace player::decode::flv1
