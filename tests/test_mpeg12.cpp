#include "decode/Frame.hpp"
#include "decode/mpeg12/Mpeg12BitReader.hpp"
#include "decode/mpeg12/Mpeg12Decoder.hpp"
#include "decode/mpeg12/Mpeg12Idct.hpp"
#include "decode/mpeg12/Mpeg12Motion.hpp"
#include "decode/mpeg12/Mpeg12Tables.hpp"
#include "decode/mpeg12/Mpeg12Vlc.hpp"

#include <gtest/gtest.h>

namespace {

using namespace player::decode;
using namespace player::decode::mpeg12;

// Big-endian bit writer used to build synthetic bitstreams.
class BitWriter {
public:
    void put(uint32_t value, unsigned bits) {
        for (int i = static_cast<int>(bits) - 1; i >= 0; --i) {
            buffer_ = static_cast<uint8_t>((buffer_ << 1) | ((value >> i) & 1u));
            if (++count_ == 8) {
                bytes_.push_back(buffer_);
                buffer_ = 0;
                count_ = 0;
            }
        }
    }

    // Writes a codeword of `bits` length, MSB first.
    void putCode(uint32_t code, unsigned bits) { put(code, bits); }

    // Pads to the next byte boundary with zero bits.
    void align() {
        while (count_ != 0) {
            put(0, 1);
        }
    }

    // Flushes any partial byte and returns the stream.
    std::vector<uint8_t> bytes() {
        align();
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    uint8_t buffer_ = 0;
    unsigned count_ = 0;
};

void appendStartCode(std::vector<uint8_t>& out, uint8_t code) {
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
    out.push_back(code);
}

uint32_t lumaSample(const Frame& frame, int x, int y) {
    return frame.plane(0)[static_cast<size_t>(y) * frame.stride_ + x];
}

}  // namespace

// ---------------------------------------------------------------------------
// BitReader
// ---------------------------------------------------------------------------

TEST(Mpeg1BitReader, ReadsMostSignificantBitFirst) {
    const uint8_t data[] = {0xA5, 0x0F, 0xFF};
    BitReader reader(data, sizeof(data));

    EXPECT_EQ(reader.read(1), 1u);
    EXPECT_EQ(reader.read(3), 0b010u);
    EXPECT_EQ(reader.read(4), 0b0101u);
    EXPECT_EQ(reader.read(8), 0x0Fu);
    EXPECT_EQ(reader.read(8), 0xFFu);
    EXPECT_FALSE(reader.error());
    EXPECT_EQ(reader.bitsLeft(), 0u);
}

TEST(Mpeg1BitReader, PeekDoesNotAdvance) {
    const uint8_t data[] = {0xB0, 0x00};
    BitReader reader(data, sizeof(data));
    EXPECT_EQ(reader.peek(4), 0xBu);
    EXPECT_EQ(reader.peek(4), 0xBu);
    EXPECT_EQ(reader.bitPosition(), 0u);
    EXPECT_EQ(reader.read(4), 0xBu);
    EXPECT_EQ(reader.bitPosition(), 4u);
}

TEST(Mpeg1BitReader, ReadsTwosComplement) {
    // 0x5A is 01011010, so the first four bits are 01 01, both +1 in two's
    // complement.
    const uint8_t data[] = {0x5A, 0xA5, 0xC3};
    BitReader reader(data, sizeof(data));
    EXPECT_EQ(reader.readSigned(2), 1);
    EXPECT_EQ(reader.readSigned(2), 1);
    // Four bits of 0x5A are left, so the next eight bits are the tail of 0x5A
    // followed by the top half of 0xA5: 1010 1010, which is negative.
    EXPECT_EQ(reader.readSigned(8), -86);
    // The final six bits are 010111.
    EXPECT_EQ(reader.readSigned(6), 23);
}

TEST(Mpeg1BitReader, ReadingPastTheEndRaisesTheErrorFlag) {
    const uint8_t data[] = {0xFF};
    BitReader reader(data, sizeof(data));
    EXPECT_EQ(reader.read(8), 0xFFu);
    EXPECT_FALSE(reader.error());
    EXPECT_EQ(reader.read(1), 0u);
    EXPECT_TRUE(reader.error());
}

TEST(Mpeg1BitReader, SkipAllOnesRequiresOneBits) {
    // Three bits consumed leaves five padding bits that must all be one.
    {
        const uint8_t all_ones[] = {0xFF};
        BitReader reader(all_ones, sizeof(all_ones));
        reader.read(3);
        EXPECT_TRUE(reader.skipAllOnes());
        EXPECT_TRUE(reader.byteAligned());
    }
    {
        const uint8_t with_zero[] = {0xF8};
        BitReader reader(with_zero, sizeof(with_zero));
        reader.read(3);
        EXPECT_FALSE(reader.skipAllOnes());
    }
}

// ---------------------------------------------------------------------------
// tables
// ---------------------------------------------------------------------------

// Every code table except the macroblock_type ones must be a prefix code, so
// that a truncated stream cannot decode as a longer, unrelated codeword.
TEST(Mpeg1Tables, PrefixTablesHaveUniqueCodewords) {
    const struct {
        const char* name;
        const VlcEntry* table;
        size_t count;
    } tables[] = {
        {"mbAddrIncr", kMbAddrIncr, kMbAddrIncrCount},
        {"cbp", kCodedBlockPattern, kCbpCount},
        {"motionVector", kMotionVector, kMotionVectorCount},
        {"dcLuma", kDcLuma, kDcCount},
        {"dcChroma", kDcChroma, kDcCount},
        {"runLevel", kRunLevel, kRunLevelCount + 2},
    };
    for (const auto& entry : tables) {
        for (size_t i = 0; i < entry.count; ++i) {
            for (size_t j = i + 1; j < entry.count; ++j) {
                ASSERT_GT(entry.table[i].bits, 0u) << entry.name;
                ASSERT_GT(entry.table[j].bits, 0u) << entry.name;
                EXPECT_FALSE(entry.table[i].bits == entry.table[j].bits &&
                             entry.table[i].code == entry.table[j].code)
                    << entry.name << " entries " << i << " and " << j;
            }
        }
    }
}

TEST(Mpeg1Tables, ZigZagIsAPermutationAndStartsAtZero) {
    bool seen[64] = {};
    for (int i = 0; i < 64; ++i) {
        ASSERT_GE(kZigZag[i], 0);
        ASSERT_LT(kZigZag[i], 64);
        EXPECT_FALSE(seen[kZigZag[i]]) << "duplicate index " << kZigZag[i];
        seen[kZigZag[i]] = true;
    }
    EXPECT_EQ(kZigZag[0], 0);
    EXPECT_EQ(kZigZag[1], 1);
    EXPECT_EQ(kZigZag[2], 8);
    EXPECT_EQ(kZigZag[3], 16);
}

TEST(Mpeg1Tables, IntraDcEntryUsesEight) {
    // The default intra matrix scales the DC predictor by 8, which is what makes
    // a predictor of 128 come back out of the transform as 128.
    EXPECT_EQ(kDefaultIntraMatrix[0], 8);
    EXPECT_EQ(kDefaultNonIntraMatrix[0], 16);
}

// ---------------------------------------------------------------------------
// VLC decoding
// ---------------------------------------------------------------------------

TEST(Mpeg1Vlc, MatchesTheLongestCodewordFirst) {
    // The one bit codeword `1` is an increment of zero.
    {
        BitWriter writer;
        writer.putCode(0x1, 1);
        writer.putCode(0, 7);
        const std::vector<uint8_t> bytes = writer.bytes();
        BitReader reader(bytes.data(), bytes.size());
        int increment = 0;
        bool end_of_slice = false;
        ASSERT_TRUE(readMbAddressIncrement(reader, increment, end_of_slice));
        EXPECT_EQ(increment, 0);
        EXPECT_FALSE(end_of_slice);
    }
    // `010` is an increment of two.
    {
        BitWriter writer;
        writer.putCode(0x2, 3);
        writer.putCode(0, 5);
        const std::vector<uint8_t> bytes = writer.bytes();
        BitReader reader(bytes.data(), bytes.size());
        int increment = 0;
        bool end_of_slice = false;
        ASSERT_TRUE(readMbAddressIncrement(reader, increment, end_of_slice));
        EXPECT_EQ(increment, 2);
    }
}

TEST(Mpeg1Vlc, MacroblockAddressIncrementHandlesEscapeAndStuffing) {
    // One escape (adds 33), a stuffing entry (adds nothing) and then 2.
    BitWriter writer;
    writer.putCode(kMbAddrIncr[kMbAddrIncrEscape].code,
                   kMbAddrIncr[kMbAddrIncrEscape].bits);
    writer.putCode(kMbAddrIncr[kMbAddrIncrStuffing].code,
                   kMbAddrIncr[kMbAddrIncrStuffing].bits);
    writer.putCode(0x2, 3);
    writer.putCode(0, 5);
    const std::vector<uint8_t> bytes = writer.bytes();

    BitReader reader(bytes.data(), bytes.size());
    int increment = 0;
    bool end_of_slice = false;
    ASSERT_TRUE(readMbAddressIncrement(reader, increment, end_of_slice));
    EXPECT_EQ(increment, 35);
    EXPECT_FALSE(end_of_slice);
    EXPECT_FALSE(reader.error());
}

TEST(Mpeg1Vlc, MacroblockAddressIncrementReportsTheSliceTerminator) {
    BitWriter writer;
    writer.putCode(kMbAddrIncr[kMbAddrIncrEnd].code, kMbAddrIncr[kMbAddrIncrEnd].bits);
    const std::vector<uint8_t> bytes = writer.bytes();

    BitReader reader(bytes.data(), bytes.size());
    int increment = 0;
    bool end_of_slice = false;
    ASSERT_TRUE(readMbAddressIncrement(reader, increment, end_of_slice));
    EXPECT_TRUE(end_of_slice);
    EXPECT_EQ(increment, 0);
}

TEST(Mpeg1Vlc, CodedBlockPatternSymbolIsThePatternItself) {
    BitWriter writer;
    // Pattern 63 is the six bit codeword 001100.
    writer.putCode(0x0C, 6);
    const std::vector<uint8_t> bytes = writer.bytes();

    BitReader reader(bytes.data(), bytes.size());
    EXPECT_EQ(readCodedBlockPattern(reader), 63);
}

TEST(Mpeg1Vlc, PMacroblockTypeUsesLongestMatch) {
    // macroblock_type `001` (forward motion, no pattern) is a prefix of the
    // five bit codeword `00100`, so the longest match must win.
    {
        BitWriter writer;
        writer.putCode(0x1, 3);  // 001
        writer.putCode(0, 5);     // trailing bits of the next macroblock
        const std::vector<uint8_t> bytes = writer.bytes();
        BitReader reader(bytes.data(), bytes.size());
        EXPECT_EQ(readMbTypeP(reader), 2);
    }
    {
        BitWriter writer;
        writer.putCode(0x2, 5);  // 00100
        writer.putCode(0, 3);
        const std::vector<uint8_t> bytes = writer.bytes();
        BitReader reader(bytes.data(), bytes.size());
        EXPECT_EQ(readMbTypeP(reader), 6);
    }
    {
        BitWriter writer;
        writer.putCode(0x1, 1);  // `1` is forward motion with a pattern
        writer.putCode(0, 7);
        const std::vector<uint8_t> bytes = writer.bytes();
        BitReader reader(bytes.data(), bytes.size());
        EXPECT_EQ(readMbTypeP(reader), 3);
    }
}

TEST(Mpeg1Vlc, BMacroblockTypesMapToTheirFlags) {
    // `10` is forward and backward with no pattern, which is index 5.
    BitWriter writer;
    writer.putCode(0x2, 2);
    writer.putCode(0, 6);
    const std::vector<uint8_t> bytes = writer.bytes();
    BitReader reader(bytes.data(), bytes.size());
    const int type = readMbTypeB(reader);
    ASSERT_GE(type, 0);
    EXPECT_FALSE(kMbTypeB[type].intra);
    EXPECT_TRUE(kMbTypeB[type].motion_forward);
    EXPECT_TRUE(kMbTypeB[type].motion_backward);
    EXPECT_FALSE(kMbTypeB[type].pattern);
}

TEST(Mpeg1Vlc, ZeroEscapeLeavesTheMotionVectorUnchanged) {
    // The single bit codeword `1` is a zero delta.
    const uint8_t data[] = {0x80, 0x00};
    BitReader reader(data, sizeof(data));
    EXPECT_EQ(readMotionVector(reader, 7, 1), 7);
}

TEST(Mpeg1Vlc, MotionVectorMagnitudeUsesForwardFCode) {
    // Escape value 1 with forward_f_code 1 is a delta of 1.
    {
        const uint8_t data[] = {0x40, 0x00};  // `01` then sign 0
        BitReader reader(data, sizeof(data));
        EXPECT_EQ(readMotionVector(reader, 0, 1), 1);
    }
    // A negative sign negates the delta.
    {
        const uint8_t data[] = {0x60, 0x00};
        BitReader reader(data, sizeof(data));
        EXPECT_EQ(readMotionVector(reader, 0, 1), -1);
    }
    // With forward_f_code 3 the magnitude is (d - 1) * 4 + extra + 1.
    {
        const uint8_t data[] = {0x40, 0x30, 0x00};
        BitReader reader(data, sizeof(data));
        EXPECT_EQ(readMotionVector(reader, 0, 3), 1);
    }
}

TEST(Mpeg1Vlc, IntraDcDifferenceUsesTheSymbolAsBitCount) {
    // Luma codeword `100` is symbol 0, i.e. a difference of zero.
    {
        const uint8_t data[] = {0x80, 0x00};
        BitReader reader(data, sizeof(data));
        EXPECT_EQ(readIntraDcDifference(reader, 0), 0);
    }
    // Luma codeword `01` is symbol 2, so two differential bits follow. The
    // differential is not two's complement: a leading zero subtracts 2^2 - 1,
    // so 01 is -2 while 10 is +2.
    {
        BitWriter writer;
        writer.putCode(0x1, 2);
        writer.putCode(0b01, 2);
        const std::vector<uint8_t> bytes = writer.bytes();
        BitReader reader(bytes.data(), bytes.size());
        EXPECT_EQ(readIntraDcDifference(reader, 0), -2);
    }
    {
        BitWriter writer;
        writer.putCode(0x1, 2);
        writer.putCode(0b10, 2);
        const std::vector<uint8_t> bytes = writer.bytes();
        BitReader reader(bytes.data(), bytes.size());
        EXPECT_EQ(readIntraDcDifference(reader, 0), 2);
    }
    // A leading zero means value - (2^size - 1), which is how an encoder writes
    // a large negative difference. `111110` is size 7, and 0001111 is
    // 15 - 127 = -112, taking the predictor from 128 down to 16.
    {
        BitWriter writer;
        writer.putCode(0x3e, 6);
        writer.putCode(0b0001111, 7);
        const std::vector<uint8_t> bytes = writer.bytes();
        BitReader reader(bytes.data(), bytes.size());
        EXPECT_EQ(readIntraDcDifference(reader, 0), -112);
    }
}

TEST(Mpeg1Vlc, AcCoefficientCarriesRunLevelAndSign) {
    // Index 0 is run 0, level 1, with a single sign bit.
    BitWriter writer;
    writer.putCode(0x3, 2);  // 11
    writer.putCode(1, 1);    // negative
    writer.putCode(0x2, 2);  // end of block
    const std::vector<uint8_t> bytes = writer.bytes();

    BitReader reader(bytes.data(), bytes.size());
    int index = 0;
    int level = 0;
    ASSERT_TRUE(readAcCoefficient(reader, AcTable::Mpeg1, index, level));
    EXPECT_EQ(index, 1);
    EXPECT_EQ(level, -1);
    EXPECT_FALSE(readAcCoefficient(reader, AcTable::Mpeg1, index, level));
    EXPECT_FALSE(reader.error());
}

TEST(Mpeg1Vlc, AcEscapeCarriesRunAndWideLevel) {
    BitWriter writer;
    writer.putCode(0x1, 6);  // escape
    writer.putCode(4, 6);    // run 5
    writer.putCode(0xFF, 8); // level -1 in eight bits
    writer.putCode(0x2, 2);  // end of block
    const std::vector<uint8_t> bytes = writer.bytes();

    BitReader reader(bytes.data(), bytes.size());
    int index = -1;
    int level = 0;
    ASSERT_TRUE(readAcCoefficient(reader, AcTable::Mpeg1, index, level));
    // The escape adds run + 1 to a position that starts before the DC.
    EXPECT_EQ(index, 4);
    EXPECT_EQ(level, -1);
}

// ---------------------------------------------------------------------------
// inverse quantisation and the IDCT
// ---------------------------------------------------------------------------

TEST(Mpeg1Idct, ReconstructedCoefficientsAreOdd) {
    // Intra AC divides by 16, so level 1 at scale 32 with a matrix entry of 16
    // is 32, which is then forced down to the odd value 31.
    const int value = inverseQuantize(1, 32, kDefaultIntraMatrix[1], true);
    EXPECT_EQ(value, 31);
    EXPECT_EQ(value % 2, 1);
    // Non-intra uses (2 * level + 1) divided by 32: 1536 >> 5 is 48, rounded
    // down to the odd value 47.
    const int inter = inverseQuantize(1, 32, kDefaultNonIntraMatrix[1], false);
    EXPECT_EQ(inter, 47);
    EXPECT_EQ(inter % 2, 1);
    // The default quantiser scale code of 1 means a scale of 2.
    EXPECT_EQ(inverseQuantize(1, 2, kDefaultIntraMatrix[1], true), 1);
}

TEST(Mpeg1Idct, SignIsPreserved) {
    EXPECT_EQ(inverseQuantize(-1, 32, kDefaultIntraMatrix[1], true), -31);
    EXPECT_EQ(inverseQuantize(-1, 32, kDefaultNonIntraMatrix[1], false), -47);
}

TEST(Mpeg1Idct, DcOnlyBlockTransformsToAFlatValue) {
    // An intra DC predictor of 128 scaled by the matrix entry of 8 must come
    // back out of the transform as exactly 128.
    Block block;
    clearBlock(block);
    block.coef[0] = static_cast<int16_t>(128 * kDefaultIntraMatrix[0]);

    uint8_t pixels[64] = {};
    idctPutBlock(block, pixels, 8);
    for (int i = 0; i < 64; ++i) {
        ASSERT_EQ(pixels[i], 128) << "sample " << i;
    }
}

TEST(Mpeg1Idct, AddOverTheExistingPrediction) {
    Block block;
    clearBlock(block);
    block.coef[0] = static_cast<int16_t>(128 * kDefaultIntraMatrix[0]);

    uint8_t pixels[64];
    std::memset(pixels, 40, sizeof(pixels));
    idctAddBlock(block, pixels, 8);
    for (int i = 0; i < 64; ++i) {
        ASSERT_EQ(pixels[i], 168) << "sample " << i;
    }
}

TEST(Mpeg1Idct, ClipsToEightBits) {
    // A DC large enough to land past each end of the range. Setting every
    // coefficient would not do it: with a correct transform the AC terms
    // cancel rather than summing to a constant.
    {
        Block block;
        clearBlock(block);
        block.coef[0] = 4096;
        uint8_t pixels[64] = {};
        idctPutBlock(block, pixels, 8);
        for (int i = 0; i < 64; ++i) {
            EXPECT_EQ(pixels[i], 255) << "sample " << i;
        }
    }
    {
        Block block;
        clearBlock(block);
        block.coef[0] = -4096;
        uint8_t pixels[64] = {};
        idctPutBlock(block, pixels, 8);
        for (int i = 0; i < 64; ++i) {
            EXPECT_EQ(pixels[i], 0) << "sample " << i;
        }
    }
}

TEST(Mpeg1Idct, AcCoefficientVariesAcrossTheBlock) {
    // A single AC coefficient has to vary with the output position. A transform
    // that ignores the output index makes the block constant instead, which
    // leaves DC-only pictures looking right and everything else wrong.
    Block block;
    clearBlock(block);
    block.coef[0] = 1024;  // flat mid grey of 128
    block.coef[1] = 512;   // first horizontal frequency
    uint8_t pixels[64] = {};
    idctPutBlock(block, pixels, 8);
    EXPECT_NE(pixels[0], pixels[7]);
    // The horizontal pass must vary along a row, not along a column.
    EXPECT_EQ(pixels[0], pixels[8 * 7]);
}

// ---------------------------------------------------------------------------
// motion compensation
// ---------------------------------------------------------------------------

TEST(Mpeg1Motion, FullPelCopiesTheReference) {
    Frame reference;
    ASSERT_TRUE(reference.alloc(16, 16, ChromaFormat::Mono));
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            reference.plane(0)[y * 16 + x] = static_cast<uint8_t>(y * 16 + x);
        }
    }
    Frame destination;
    ASSERT_TRUE(destination.alloc(16, 16, ChromaFormat::Mono));
    std::memset(destination.plane(0), 0, 256);

    ASSERT_TRUE(predictBlock(frameGrid(destination, 0), frameGrid(reference, 0), 0, 0, 16, 16, 0, 0));
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            EXPECT_EQ(destination.plane(0)[y * 16 + x], static_cast<uint8_t>(y * 16 + x));
        }
    }
}

TEST(Mpeg1Motion, HalfPelAveragesUpwards) {
    Frame reference;
    ASSERT_TRUE(reference.alloc(16, 16, ChromaFormat::Mono));
    std::memset(reference.plane(0), 10, 256);
    // Make one column odd so the rounding direction is visible.
    for (int y = 0; y < 16; ++y) {
        reference.plane(0)[y * 16 + 8] = 11;
    }

    Frame destination;
    ASSERT_TRUE(destination.alloc(16, 16, ChromaFormat::Mono));

    // Horizontal half sample at column 8 averages 11 and 10.
    ASSERT_TRUE(
        predictBlock(frameGrid(destination, 0), frameGrid(reference, 0), 8, 0, 4, 4, 1, 0));
    for (int y = 0; y < 4; ++y) {
        EXPECT_EQ(destination.plane(0)[y * 16 + 8], 11);  // (11 + 10 + 1) >> 1
    }
}

TEST(Mpeg1Motion, ReplicatesTheEdgeForVectorsOutsideTheReference) {
    Frame reference;
    ASSERT_TRUE(reference.alloc(16, 16, ChromaFormat::Mono));
    // A column of 40 at the right edge and a row of 60 at the bottom edge, so
    // clamping is distinguishable from reading whatever follows the picture.
    std::memset(reference.plane(0), 10, 256);
    for (int y = 0; y < 16; ++y) {
        reference.plane(0)[y * 16 + 15] = 40;
    }
    for (int x = 0; x < 16; ++x) {
        reference.plane(0)[15 * 16 + x] = 60;
    }
    reference.plane(0)[15 * 16 + 15] = 90;

    Frame destination;
    ASSERT_TRUE(destination.alloc(16, 16, ChromaFormat::Mono));

    // Two pixels past the right edge: the whole block reads the 40 column, so
    // the first column of the destination must be 40 rather than 10.
    ASSERT_TRUE(predictBlock(frameGrid(destination, 0), frameGrid(reference, 0), 0, 0, 16, 16,
                             2 * 16, 0));
    EXPECT_EQ(destination.plane(0)[0], 40);

    // Past the bottom edge: the 60 row, and the corner where both clamp is 90.
    std::memset(destination.plane(0), 0, 256);
    ASSERT_TRUE(predictBlock(frameGrid(destination, 0), frameGrid(reference, 0), 0, 0, 16, 16, 0,
                             2 * 16));
    EXPECT_EQ(destination.plane(0)[0], 60);
    EXPECT_EQ(destination.plane(0)[15], 90);

    // Past the left edge: column 0, which is 10.
    std::memset(destination.plane(0), 0, 256);
    ASSERT_TRUE(predictBlock(frameGrid(destination, 0), frameGrid(reference, 0), 0, 0, 16, 16, -2,
                             0));
    EXPECT_EQ(destination.plane(0)[0], 10);
}

// ---------------------------------------------------------------------------
// end to end
// ---------------------------------------------------------------------------

namespace {

// A 16x16 single macroblock I picture whose six blocks all carry a zero DC
// difference, so every block decodes to the flat grey 128.
std::vector<uint8_t> buildIntraFrame() {
    std::vector<uint8_t> out;

    // sequence_header: 16x16, square pixels, 25 fps.
    appendStartCode(out, 0xB3);
    {
        BitWriter writer;
        writer.put(16, 12);   // horizontal_size
        writer.put(16, 12);   // vertical_size
        writer.put(1, 4);     // aspect_ratio_information
        writer.put(3, 4);     // frame_rate_code, 25 fps
        writer.put(0x3FFFF, 18);  // bit_rate, variable
        writer.put(1, 1);     // marker_bit
        writer.put(0, 8);     // vbv_buffer_size
        writer.put(0, 1);     // constrained_parameters_flag
        writer.put(0, 1);     // load_intra_quantiser_matrix
        writer.put(0, 1);     // load_non_intra_quantiser_matrix
        writer.align();
        const std::vector<uint8_t> bytes = writer.bytes();
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    // picture_header: an I picture.
    appendStartCode(out, 0x00);
    {
        BitWriter writer;
        writer.put(0, 10);        // temporal_reference
        writer.put(1, 3);         // picture_coding_type, intra
        writer.put(0xFFFF, 16);   // vbv_delay
        writer.put(1, 1);         // extra_bit_picture
        writer.align();
        const std::vector<uint8_t> bytes = writer.bytes();
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    // One slice covering the single macroblock row.
    appendStartCode(out, 0x01);
    {
        BitWriter writer;
        writer.put(16, 5);  // quantiser_scale_code
        writer.put(0, 1);   // skip_1stop_8data_bits stop bit
        writer.putCode(0x1, 1);  // macroblock_address_increment of 0

        writer.putCode(0x1, 1);   // macroblock_type: intra, quantiser unchanged
        for (int block = 0; block < 6; ++block) {
            if (block == 0 || block >= 4) {
                // DC difference of +8 so the picture cannot be confused with
                // the neutral fill, and so every intra_dc_precision lands on a
                // distinguishable value. A difference is DPCM and Cb and Cr
                // have separate predictors, so the first luma block and both
                // chroma blocks carry it.
                if (block == 0) {
                    writer.putCode(0x6, 3);    // luma DC symbol 4
                    writer.put(0b1000, 4);     // +8
                } else {
                    writer.putCode(0xe, 4);    // chroma DC symbol 4
                    writer.put(0b1000, 4);     // +8
                }
            } else {
                writer.putCode(0x4, 3);        // luma DC difference of 0
            }
            writer.putCode(0x2, 2);            // end_of_block
        }
        writer.align();
        const std::vector<uint8_t> bytes = writer.bytes();
        out.insert(out.end(), bytes.begin(), bytes.end());
    }

    appendStartCode(out, 0xB7);  // sequence_end_code
    return out;
}

}  // namespace

TEST(Mpeg1Decoder, DecodesAFlatGreyIntraPicture) {
    const std::vector<uint8_t> stream = buildIntraFrame();

    Decoder decoder;
    decoder.pushSample(stream.data(), stream.size(), 0);

    std::vector<DecodedFrame> frames;
    decoder.flush(frames);

    ASSERT_EQ(frames.size(), 1u);
    const Frame& frame = frames[0].image;
    ASSERT_TRUE(frame.valid());
    EXPECT_EQ(frame.width_, 16);
    EXPECT_EQ(frame.height_, 16);
    EXPECT_EQ(frames[0].slice_type, SliceType::I);
    EXPECT_FALSE(decoder.damaged());

    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            ASSERT_EQ(lumaSample(frame, x, y), 136) << "luma at " << x << "," << y;
        }
    }
    for (int plane = 1; plane < 3; ++plane) {
        for (int y = 0; y < frame.height_c_; ++y) {
            for (int x = 0; x < frame.width_c_; ++x) {
                ASSERT_EQ(frame.plane(plane)[y * frame.stride_c_ + x], 136)
                    << "chroma plane " << plane << " at " << x << "," << y;
            }
        }
    }
}

TEST(Mpeg1Decoder, ReportsGeometryFromTheSequenceHeader) {
    const std::vector<uint8_t> stream = buildIntraFrame();
    Decoder decoder;
    decoder.pushSample(stream.data(), stream.size(), 0);

    EXPECT_TRUE(decoder.haveSequence());
    EXPECT_EQ(decoder.width(), 16u);
    EXPECT_EQ(decoder.height(), 16u);
    EXPECT_DOUBLE_EQ(decoder.fps(), 25.0);
}

TEST(Mpeg1Decoder, ReassemblesAStreamSplitAcrossSamples) {
    const std::vector<uint8_t> stream = buildIntraFrame();

    // Feed one byte at a time, which splits every header and the slice.
    Decoder decoder;
    for (size_t i = 0; i < stream.size(); ++i) {
        decoder.pushSample(stream.data() + i, 1, static_cast<int64_t>(i));
    }

    std::vector<DecodedFrame> frames;
    decoder.flush(frames);
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames[0].image.width_, 16);
    EXPECT_FALSE(decoder.damaged());
}

TEST(Mpeg1Decoder, IgnoresAPictureWithoutASequenceHeader) {
    // A picture header with no sequence header has no geometry, so nothing can
    // be decoded.
    std::vector<uint8_t> stream;
    appendStartCode(stream, 0x00);
    BitWriter writer;
    writer.put(0, 10);
    writer.put(1, 3);
    writer.put(0xFFFF, 16);
    writer.put(1, 1);
    writer.align();
    const std::vector<uint8_t> header = writer.bytes();
    stream.insert(stream.end(), header.begin(), header.end());

    Decoder decoder;
    decoder.pushSample(stream.data(), stream.size(), 0);
    std::vector<DecodedFrame> frames;
    decoder.flush(frames);
    EXPECT_TRUE(frames.empty());
    EXPECT_FALSE(decoder.haveSequence());
}

TEST(Mpeg1Decoder, ResetClearsEverything) {
    const std::vector<uint8_t> stream = buildIntraFrame();
    Decoder decoder;
    decoder.pushSample(stream.data(), stream.size(), 0);

    std::vector<DecodedFrame> frames;
    ASSERT_EQ(decoder.flush(frames), 1);

    decoder.reset();
    EXPECT_FALSE(decoder.haveSequence());
    EXPECT_EQ(decoder.width(), 0u);
    EXPECT_EQ(decoder.picturesDecoded(), 0u);

    // The decoder must work again from a clean state.
    decoder.pushSample(stream.data(), stream.size(), 0);
    frames.clear();
    EXPECT_EQ(decoder.flush(frames), 1);
    EXPECT_EQ(frames.size(), 1u);
    EXPECT_FALSE(decoder.damaged());
}

