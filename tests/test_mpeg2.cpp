// MPEG-2 (ISO/IEC 13818-2) decoding tests.
//
// There is no MPEG-2 conformance bitstream in the repository, so these build
// synthetic streams whose expected output can be computed by hand.

#include "decode/Frame.hpp"
#include "decode/mpeg12/Mpeg12Decoder.hpp"
#include "decode/mpeg12/Mpeg12Idct.hpp"
#include "decode/mpeg12/Mpeg12Tables.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using namespace player::decode;
using namespace player::decode::mpeg12;

class Bits {
public:
    void put(uint32_t value, unsigned count) {
        for (int i = static_cast<int>(count) - 1; i >= 0; --i) {
            buffer_ = static_cast<uint8_t>((buffer_ << 1) | ((value >> i) & 1u));
            if (++count_ == 8) {
                bytes_.push_back(buffer_);
                buffer_ = 0;
                count_ = 0;
            }
        }
    }

    void putCode(uint32_t code, unsigned bits) { put(code, bits); }

    std::vector<uint8_t> bytes() {
        while (count_ != 0) {
            put(0, 1);
        }
        return bytes_;
    }

private:
    std::vector<uint8_t> bytes_;
    uint8_t buffer_ = 0;
    unsigned count_ = 0;
};

void startCode(std::vector<uint8_t>& out, uint8_t code) {
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
    out.push_back(code);
}

void append(std::vector<uint8_t>& out, const std::vector<uint8_t>& data) {
    out.insert(out.end(), data.begin(), data.end());
}

struct PictureOptions {
    int intra_dc_precision = 0;
    int picture_structure = 3;  // 3 = frame
    bool alternate_scan = false;
    bool intra_vlc_format = false;
    bool q_scale_type = false;
    int quantiser_scale_code = 8;
    int temporal_reference = 0;
};

// A 16x16 single macroblock I picture whose blocks all carry a zero DC
// difference, so the reconstructed picture is flat grey 128.
std::vector<uint8_t> buildMpeg2IntraFrame(const PictureOptions& options = {}) {
    std::vector<uint8_t> out;

    // sequence_header: 16x16, square pixels, 25 fps.
    startCode(out, 0xB3);
    {
        Bits bits;
        bits.put(16, 12);
        bits.put(16, 12);
        bits.put(1, 4);
        bits.put(3, 4);
        bits.put(0x3FFFF, 18);
        bits.put(1, 1);    // marker_bit
        bits.put(0, 10);   // vbv_buffer_size
        bits.put(0, 1);    // constrained_parameters_flag
        bits.put(0, 1);    // load_intra_quantiser_matrix
        bits.put(0, 1);    // load_non_intra_quantiser_matrix
        append(out, bits.bytes());
    }

    // sequence_extension: Main Profile at Main Level, progressive, 4:2:0.
    startCode(out, 0xB5);
    {
        Bits bits;
        bits.put(1, 4);      // extension id: sequence
        bits.put(0x48, 8);   // profile_and_level_indication
        bits.put(1, 1);      // progressive_sequence
        bits.put(1, 2);      // chroma_format: 4:2:0
        bits.put(0, 2);      // horizontal_size_extension
        bits.put(0, 2);      // vertical_size_extension
        bits.put(0, 12);     // bit_rate_extension
        bits.put(1, 1);      // marker_bit
        bits.put(0, 8);      // vbv_buffer_size_extension
        bits.put(0, 1);      // low_delay
        bits.put(0, 2);      // frame_rate_extension_n
        bits.put(0, 5);      // frame_rate_extension_d
        append(out, bits.bytes());
    }

    // picture_header.
    startCode(out, 0x00);
    {
        Bits bits;
        bits.put(static_cast<uint32_t>(options.temporal_reference), 10);
        bits.put(1, 3);      // picture_coding_type: I
        bits.put(0xFFFF, 16);  // vbv_delay
        bits.put(0, 1);      // extra_bit_picture
        append(out, bits.bytes());
    }

    // picture_coding_extension.
    startCode(out, 0xB5);
    {
        Bits bits;
        bits.put(8, 4);      // extension id: picture coding
        for (int i = 0; i < 4; ++i) {
            bits.put(0xF, 4);  // f_code, unused for I pictures
        }
        bits.put(static_cast<uint32_t>(options.intra_dc_precision), 2);
        bits.put(static_cast<uint32_t>(options.picture_structure), 2);
        bits.put(0, 1);      // top_field_first
        bits.put(1, 1);      // frame_pred_frame_dct
        bits.put(0, 1);      // concealment_motion_vectors
        bits.put(options.q_scale_type ? 1 : 0, 1);
        bits.put(options.intra_vlc_format ? 1 : 0, 1);
        bits.put(options.alternate_scan ? 1 : 0, 1);
        bits.put(0, 1);      // repeat_first_field
        bits.put(0, 1);      // chroma_420_type
        bits.put(1, 1);      // progressive_frame
        bits.put(0, 1);      // composite_display_flag
        append(out, bits.bytes());
    }

    // One slice covering the single macroblock row.
    startCode(out, 0x01);
    {
        Bits bits;
        bits.put(static_cast<uint32_t>(options.quantiser_scale_code), 5);
        bits.put(0, 1);   // intra_slice_flag
        bits.put(1, 1);   // extra_bit_slice
        bits.put(1, 1);   // skip_1stop_8data_bits
        bits.put(1, 1);   // macroblock_address_increment of 0
        bits.put(1, 1);   // macroblock_type: intra, quantiser unchanged
        for (int block = 0; block < 6; ++block) {
            bits.put(4, 3);  // luma DC difference of 0
            bits.put(0, 2);  // chroma DC difference of 0
            bits.put(2, 2);  // end_of_block
        }
        append(out, bits.bytes());
    }

    startCode(out, 0xB7);
    return out;
}

uint32_t luma(const Frame& frame, int x, int y) {
    return frame.plane(0)[static_cast<size_t>(y) * frame.stride_ + x];
}

}  // namespace

TEST(Mpeg2, SequenceExtensionSelectsTheStandard) {
    const std::vector<uint8_t> stream = buildMpeg2IntraFrame();
    Decoder decoder;
    decoder.pushSample(stream.data(), stream.size(), 0);

    EXPECT_EQ(decoder.standard(), Standard::Mpeg2);
    EXPECT_TRUE(decoder.progressiveSequence());
    EXPECT_FALSE(decoder.interlaced());
    EXPECT_EQ(decoder.width(), 16u);
    EXPECT_EQ(decoder.height(), 16u);
    EXPECT_DOUBLE_EQ(decoder.fps(), 25.0);
}

TEST(Mpeg2, DecodesAFlatGreyIntraPicture) {
    const std::vector<uint8_t> stream = buildMpeg2IntraFrame();
    Decoder decoder;
    decoder.pushSample(stream.data(), stream.size(), 0);

    std::vector<DecodedFrame> frames;
    decoder.flush(frames);

    ASSERT_EQ(frames.size(), 1u);
    const Frame& frame = frames[0].image;
    ASSERT_TRUE(frame.valid());
    EXPECT_FALSE(decoder.damaged());
    EXPECT_EQ(frames[0].slice_type, SliceType::I);

    // Mismatch control toggles coefficient 63, but its contribution to the
    // inverse transform is under half a sample, so the picture stays flat.
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            ASSERT_EQ(luma(frame, x, y), 128) << "at " << x << "," << y;
        }
    }
    for (int plane = 1; plane < 3; ++plane) {
        for (int y = 0; y < frame.height_c_; ++y) {
            for (int x = 0; x < frame.width_c_; ++x) {
                ASSERT_EQ(frame.plane(plane)[y * frame.stride_c_ + x], 128);
            }
        }
    }
}

TEST(Mpeg2, IntraDcPrecisionKeepsMidGreyAtMidGrey) {
    // With intra_dc_precision p the predictor starts at 128 << p and the level
    // is scaled back by 3 - p, so a zero difference must still give 128.
    for (int precision = 0; precision < 4; ++precision) {
        PictureOptions options;
        options.intra_dc_precision = precision;
        const std::vector<uint8_t> stream = buildMpeg2IntraFrame(options);

        Decoder decoder;
        decoder.pushSample(stream.data(), stream.size(), 0);
        std::vector<DecodedFrame> frames;
        decoder.flush(frames);
        ASSERT_EQ(frames.size(), 1u) << "precision " << precision;
        EXPECT_FALSE(decoder.damaged()) << "precision " << precision;
        EXPECT_EQ(luma(frames[0].image, 0, 0), 128) << "precision " << precision;
        EXPECT_EQ(luma(frames[0].image, 15, 15), 128) << "precision " << precision;
    }
}

TEST(Mpeg2, AlternateScanAndIntraVlcFormatAreAccepted) {
    PictureOptions options;
    options.alternate_scan = true;
    options.intra_vlc_format = true;
    const std::vector<uint8_t> stream = buildMpeg2IntraFrame(options);

    Decoder decoder;
    decoder.pushSample(stream.data(), stream.size(), 0);
    std::vector<DecodedFrame> frames;
    decoder.flush(frames);

    ASSERT_EQ(frames.size(), 1u);
    EXPECT_FALSE(decoder.damaged());
    EXPECT_EQ(luma(frames[0].image, 0, 0), 128);
}

TEST(Mpeg2, NonLinearQuantiserScaleIsAccepted) {
    PictureOptions options;
    options.q_scale_type = true;
    options.quantiser_scale_code = 16;
    const std::vector<uint8_t> stream = buildMpeg2IntraFrame(options);

    Decoder decoder;
    decoder.pushSample(stream.data(), stream.size(), 0);
    std::vector<DecodedFrame> frames;
    decoder.flush(frames);

    ASSERT_EQ(frames.size(), 1u);
    EXPECT_FALSE(decoder.damaged());
    EXPECT_EQ(luma(frames[0].image, 0, 0), 128);
}

TEST(Mpeg2, FieldPictureIsRecognisedAsInterlaced) {
    PictureOptions options;
    options.picture_structure = 1;  // top field
    const std::vector<uint8_t> stream = buildMpeg2IntraFrame(options);

    Decoder decoder;
    decoder.pushSample(stream.data(), stream.size(), 0);

    // A single field is held back hoping for its pair; flushing releases it.
    std::vector<DecodedFrame> frames;
    decoder.flush(frames);
    EXPECT_EQ(decoder.standard(), Standard::Mpeg2);
    EXPECT_FALSE(decoder.damaged());
}

// ---------------------------------------------------------------------------
// tables and block maths
// ---------------------------------------------------------------------------

TEST(Mpeg2Tables, RunLevelTableHasUniqueCodewords) {
    for (size_t i = 0; i < kRunLevelCount + 2; ++i) {
        ASSERT_GT(kMpeg2RunLevel[i].bits, 0u) << i;
        for (size_t j = i + 1; j < kRunLevelCount + 2; ++j) {
            EXPECT_FALSE(kMpeg2RunLevel[i].bits == kMpeg2RunLevel[j].bits &&
                         kMpeg2RunLevel[i].code == kMpeg2RunLevel[j].code)
                << "entries " << i << " and " << j;
        }
    }
}

TEST(Mpeg2Tables, RunLevelSharesTheMpeg1RunAndLevelExpansion) {
    // ffmpeg builds both AC tables over the same run/level arrays, so the
    // MPEG-2 table only reorders the codewords.
    for (size_t i = 0; i < kRunLevelCount; ++i) {
        EXPECT_LT(kRunLevelRun[i], 32u) << i;
        EXPECT_GE(kRunLevelLevel[i], 1u) << i;
    }
}

TEST(Mpeg2Tables, AlternateScanIsAPermutation) {
    bool seen[64] = {};
    for (int i = 0; i < 64; ++i) {
        ASSERT_GE(kAlternateScan[i], 0);
        ASSERT_LT(kAlternateScan[i], 64);
        EXPECT_FALSE(seen[kAlternateScan[i]]) << "duplicate " << kAlternateScan[i];
        seen[kAlternateScan[i]] = true;
    }
    EXPECT_EQ(kAlternateScan[0], 0);
    EXPECT_EQ(kAlternateScan[1], 8);
    EXPECT_NE(kAlternateScan[1], kZigZag[1]);  // it really is a different scan
}

TEST(Mpeg2Tables, NonLinearQuantiserScaleIsMonotonic) {
    EXPECT_EQ(kNonLinearQScale[0], 0);
    EXPECT_EQ(kNonLinearQScale[1], 1);
    EXPECT_EQ(kNonLinearQScale[31], 112);
    for (int i = 1; i < 32; ++i) {
        EXPECT_GE(kNonLinearQScale[i], kNonLinearQScale[i - 1]) << i;
    }
}

TEST(Mpeg2Idct, Mpeg2DoesNotForceCoefficientsOdd) {
    // Level 1 at scale 32 with matrix entry 16 is 32 for intra; MPEG-1 would
    // force it down to the odd value 31.
    EXPECT_EQ(inverseQuantize(1, 32, 16, true, true), 31);
    EXPECT_EQ(inverseQuantize(1, 32, 16, true, false), 32);
    // Non-intra: ((2 * 1 + 1) * 32 * 16) >> 5 is 48.
    EXPECT_EQ(inverseQuantize(1, 32, 16, false, true), 47);
    EXPECT_EQ(inverseQuantize(1, 32, 16, false, false), 48);
}
