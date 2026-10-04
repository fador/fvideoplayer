#include "decode/flv1/Flv1Decoder.hpp"
#include "decode/flv1/Flv1Tables.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;
using player::decode::ChromaFormat;
using player::decode::DecodedFrame;
using player::decode::flv1::Decoder;
using player::decode::mpeg12::VlcEntry;

// Writes bits MSB-first, mirroring the H.263 bitstream.
class BitWriter {
public:
    void bit(uint32_t value) {
        if ((bits_ & 7) == 0) {
            data_.push_back(0);
        }
        data_.back() = static_cast<uint8_t>(data_.back() | ((value & 1) << (7 - (bits_ & 7))));
        ++bits_;
    }

    void bits(uint32_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            bit((value >> (count - 1 - i)) & 1u);
        }
    }

    Bytes bytes() const { return data_; }

private:
    Bytes data_;
    size_t bits_ = 0;
};

// FLV1 picture header with the FLV start code, escape format, size and quantiser.
void writeHeader(BitWriter& writer, bool intra, uint8_t width, uint8_t height, int quantiser) {
    writer.bits(1, 17);  // picture start code
    writer.bits(0, 5);   // format: standard escape
    writer.bits(0, 8);   // picture timestamp
    writer.bits(0, 3);   // size code: 8-bit width and height
    writer.bits(width, 8);
    writer.bits(height, 8);
    writer.bits(intra ? 0 : 1, 2);
    writer.bits(0, 1);  // deblocking flag
    writer.bits(static_cast<uint32_t>(quantiser), 5);
    writer.bits(0, 1);  // PEI
}

// One intra macroblock with no chroma CBP and no coded luma blocks: each block
// carries only an eight-bit DC level.
void writeFlatIntraMacroblock(BitWriter& writer, uint8_t dc) {
    writer.bits(1, 1);     // intra MCBPC: cbpc = 0
    writer.bits(3, 4);     // CBPY: 0011 = 0
    for (int i = 0; i < 6; ++i) {
        writer.bits(dc, 8);
    }
}

Bytes flatIntraPicture(uint8_t width, uint8_t height, uint8_t dc, int quantiser = 8) {
    BitWriter writer;
    writeHeader(writer, true, width, height, quantiser);
    const int mb_width = (width + 15) / 16;
    const int mb_height = (height + 15) / 16;
    for (int y = 0; y < mb_height; ++y) {
        for (int x = 0; x < mb_width; ++x) {
            writeFlatIntraMacroblock(writer, dc);
        }
    }
    return writer.bytes();
}

Bytes skippedPicture(uint8_t width, uint8_t height, int quantiser = 8) {
    BitWriter writer;
    writeHeader(writer, false, width, height, quantiser);
    const int mb_width = (width + 15) / 16;
    const int mb_height = (height + 15) / 16;
    for (int i = 0; i < mb_width * mb_height; ++i) {
        writer.bits(1, 1);  // skip macroblock
    }
    return writer.bytes();
}

Bytes zeroMotionPicture(uint8_t width, uint8_t height, int quantiser = 8) {
    BitWriter writer;
    writeHeader(writer, false, width, height, quantiser);
    writer.bits(0, 1);  // not skipped
    writer.bits(1, 1);  // inter MCBPC: cbpc = 0, 16x16
    writer.bits(3, 2);  // CBPY: 11 = 15, XORed with 0xF -> no coded block
    writer.bits(1, 1);  // motion vector x: code 0, keep the predictor
    writer.bits(1, 1);  // motion vector y
    return writer.bytes();
}

std::vector<DecodedFrame> decode(Decoder& decoder, const Bytes& data) {
    decoder.pushSample(data.data(), data.size(), 0);
    std::vector<DecodedFrame> frames;
    decoder.pullFrames(frames);
    return frames;
}

uint8_t luma(const DecodedFrame& frame, int x, int y) {
    return frame.image.row(0, y)[x];
}

// True when no codeword of the table is a prefix of another.
bool prefixFree(const VlcEntry* table, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = 0; j < count; ++j) {
            if (i == j || table[i].bits > table[j].bits) {
                continue;
            }
            const uint32_t shifted = table[i].code << (table[j].bits - table[i].bits);
            if (shifted == table[j].code) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace

TEST(Flv1Tables, ArePrefixFree) {
    EXPECT_TRUE(prefixFree(player::decode::flv1::kIntraMcbpc, 9));
    EXPECT_TRUE(prefixFree(player::decode::flv1::kInterMcbpc, 25));
    EXPECT_TRUE(prefixFree(player::decode::flv1::kCbpy, 16));
    EXPECT_TRUE(prefixFree(player::decode::flv1::kMotionVlc, 33));
    EXPECT_TRUE(prefixFree(player::decode::flv1::kRunLevel, 103));
}

TEST(Flv1Tables, EscapeAndLastMarkers) {
    EXPECT_EQ(player::decode::flv1::kRunLevel[102].code, 3u);
    EXPECT_EQ(player::decode::flv1::kRunLevel[102].bits, 7);
    EXPECT_EQ(player::decode::flv1::kRunLevelLast, 58);
    // The last coefficient of the inter table has run 40 and level 1.
    EXPECT_EQ(player::decode::flv1::kRunLevelRun[101], 40);
    EXPECT_EQ(player::decode::flv1::kRunLevelLevel[101], 1);
}

TEST(Flv1Decoder, DecodesFlatIntraPicture) {
    Decoder decoder;
    const std::vector<DecodedFrame> frames = decode(decoder, flatIntraPicture(16, 16, 128));
    ASSERT_EQ(frames.size(), 1u);
    const DecodedFrame& frame = frames.front();
    EXPECT_EQ(frame.image.width_, 16);
    EXPECT_EQ(frame.image.height_, 16);
    EXPECT_EQ(frame.image.displayWidth(), 16);
    EXPECT_EQ(frame.image.displayHeight(), 16);
    EXPECT_EQ(frame.slice_type, player::decode::SliceType::I);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            EXPECT_EQ(luma(frame, x, y), 128) << x << "," << y;
        }
    }
    EXPECT_EQ(frame.image.row(1, 0)[0], 128);
    EXPECT_EQ(frame.image.row(2, 0)[0], 128);
}

TEST(Flv1Decoder, DecodesMultipleMacroblocks) {
    Decoder decoder;
    const std::vector<DecodedFrame> frames = decode(decoder, flatIntraPicture(32, 16, 90));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().image.displayWidth(), 32);
    EXPECT_EQ(luma(frames.front(), 0, 0), 90);
    EXPECT_EQ(luma(frames.front(), 31, 15), 90);
}

TEST(Flv1Decoder, PSkipCopiesReference) {
    Decoder decoder;
    ASSERT_EQ(decode(decoder, flatIntraPicture(16, 16, 100)).size(), 1u);
    const std::vector<DecodedFrame> frames = decode(decoder, skippedPicture(16, 16));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().slice_type, player::decode::SliceType::P);
    EXPECT_EQ(luma(frames.front(), 5, 5), 100);
}

TEST(Flv1Decoder, ZeroMotionPredictionCopiesReference) {
    Decoder decoder;
    ASSERT_EQ(decode(decoder, flatIntraPicture(16, 16, 77)).size(), 1u);
    const std::vector<DecodedFrame> frames = decode(decoder, zeroMotionPicture(16, 16));
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(luma(frames.front(), 8, 8), 77);
    EXPECT_EQ(frames.front().image.row(1, 0)[0], 77);
}

TEST(Flv1Decoder, SixteenBitPictureSize) {
    BitWriter writer;
    writer.bits(1, 17);
    writer.bits(0, 5);
    writer.bits(0, 8);
    writer.bits(1, 3);  // size code 1: 16-bit width and height
    writer.bits(16, 16);
    writer.bits(16, 16);
    writer.bits(0, 2);
    writer.bits(0, 1);
    writer.bits(8, 5);
    writer.bits(0, 1);
    writeFlatIntraMacroblock(writer, 55);

    Decoder decoder;
    const std::vector<DecodedFrame> frames = decode(decoder, writer.bytes());
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().image.displayWidth(), 16);
    EXPECT_EQ(luma(frames.front(), 1, 1), 55);
}

TEST(Flv1Decoder, OddPictureHeightKeepsCodedRows) {
    // 239 lines are coded as 240; the frame crops the last one.
    BitWriter writer;
    writeHeader(writer, true, 16, 239, 8);
    const int mb_height = (239 + 15) / 16;
    for (int i = 0; i < mb_height; ++i) {
        writeFlatIntraMacroblock(writer, 33);
    }
    Decoder decoder;
    const std::vector<DecodedFrame> frames = decode(decoder, writer.bytes());
    ASSERT_EQ(frames.size(), 1u);
    EXPECT_EQ(frames.front().image.height_, 240);
    EXPECT_EQ(frames.front().image.displayHeight(), 239);
    EXPECT_EQ(luma(frames.front(), 0, 239), 33);
}
