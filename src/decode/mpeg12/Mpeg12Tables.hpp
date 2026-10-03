#pragma once

#include <cstddef>
#include <cstdint>

namespace player::decode::mpeg12 {

// One entry of a variable-length code table: the codeword right aligned in
// `bits` bits, and the symbol it decodes to. MPEG-1 code tables are mostly but
// not entirely prefix codes, so they are matched longest codeword first.
struct VlcEntry {
    uint32_t code;
    uint8_t bits;
    uint8_t value;
};

// macroblock_address_increment, ISO/IEC 11172-2 Table B-1. The same table
// codes macroblock_skip_run. Symbols 0..32 are literal increments.
constexpr size_t kMbAddrIncrCount = 36;
extern const VlcEntry kMbAddrIncr[kMbAddrIncrCount];
constexpr uint8_t kMbAddrIncrEscape = 33;   // followed by another increment
constexpr uint8_t kMbAddrIncrStuffing = 34;  // consumes 11 bits, adds nothing
constexpr uint8_t kMbAddrIncrEnd = 35;       // eight zero bits end the slice

// coded_block_pattern, Table B-3. The symbol is the 6-bit pattern itself.
constexpr size_t kCbpCount = 64;
extern const VlcEntry kCodedBlockPattern[kCbpCount];

// motion_vector, Table B-14. The symbol is the escape value d in 0..16; the
// magnitude is derived from d and forward_f_code / backward_f_code.
constexpr size_t kMotionVectorCount = 17;
extern const VlcEntry kMotionVector[kMotionVectorCount];

// DC coefficient difference for intra blocks, Table B-2 (luma) and Table B-5
// (chroma). The symbol is the number of following bits; the sign is the most
// significant of those bits.
constexpr size_t kDcCount = 12;
extern const VlcEntry kDcLuma[kDcCount];
extern const VlcEntry kDcChroma[kDcCount];

// AC coefficients, Tables B-2/B-4/B-6/B-7 collapsed into one table (the three
// tables share codewords). Symbols 0..110 carry (run, level); 111 is the
// escape and 112 ends the block.
constexpr size_t kRunLevelCount = 111;
extern const VlcEntry kRunLevel[kRunLevelCount + 2];
constexpr uint8_t kRunLevelEscape = 111;
constexpr uint8_t kRunLevelEnd = 112;
// Number of leading zero coefficients the symbol skips (spec run).
extern const uint8_t kRunLevelRun[kRunLevelCount];
// Coefficient magnitude the symbol carries; the sign is a separate bit.
extern const uint8_t kRunLevelLevel[kRunLevelCount];

// macroblock_type, Table B-7 (P pictures). These are not a prefix code
// (`001` is a prefix of `00100`), so they are matched longest first.
struct MbTypeP {
    uint8_t bits;
    uint32_t code;
    uint8_t intra;
    uint8_t motion_forward;
    uint8_t pattern;    // coded_block_pattern is present
    uint8_t quant_type; // quantiser_scale_code is present
};
constexpr size_t kMbTypePCount = 7;
extern const MbTypeP kMbTypeP[kMbTypePCount];

// macroblock_type, Table B-8 (B pictures). Also not a prefix code.
struct MbTypeB {
    uint8_t bits;
    uint32_t code;
    uint8_t intra;
    uint8_t motion_forward;
    uint8_t motion_backward;
    uint8_t pattern;
    uint8_t quant_type;
};
constexpr size_t kMbTypeBCount = 11;
extern const MbTypeB kMbTypeB[kMbTypeBCount];

// Zig-zag scan of the coefficient indices, used for every block of a
// progressive picture and for every intra block.
extern const int kZigZag[64];

// Quantisation matrices as sent in the sequence header, in scan order.
extern const uint8_t kDefaultIntraMatrix[64];
extern const uint8_t kDefaultNonIntraMatrix[64];

// frame_rate_code, ISO/IEC 11172-2 Table 6-1: frames per second as a rational.
struct FrameRate {
    uint32_t numerator;
    uint32_t denominator;
};
constexpr size_t kFrameRateCount = 16;
extern const FrameRate kFrameRate[kFrameRateCount];

// pixel_aspect_ratio, Table 6-2. 0 means "unspecified".
extern const float kPixelAspectRatio[16];


// AC coefficient codes of ISO/IEC 13818-2 Table B-14 (intra_vlc_format).
extern const VlcEntry kMpeg2RunLevel[kRunLevelCount + 2];
// Alternate (vertical) scan, selected by alternate_scan.
extern const int kAlternateScan[64];
// Non-linear quantiser scale, selected by q_scale_type.
extern const uint8_t kNonLinearQScale[32];

}  // namespace player::decode::mpeg12



