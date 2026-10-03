#include "decode/Frame.hpp"
#include "decode/mpeg12/Mpeg12Decoder.hpp"
#include "demux/Demuxer.hpp"
#include "demux/Esds.hpp"
#include "demux/RawMpegDemuxer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace player::demux;
using namespace player::decode;

std::filesystem::path writeBytes(const std::vector<uint8_t>& data, const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path;
}

void appendStartCode(std::vector<uint8_t>& out, uint8_t code) {
    out.push_back(0x00);
    out.push_back(0x00);
    out.push_back(0x01);
    out.push_back(code);
}

// Big-endian bit writer, so the tests can state the expected bitstream.
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

void appendBytes(std::vector<uint8_t>& out, const std::vector<uint8_t>& data) {
    out.insert(out.end(), data.begin(), data.end());
}

void appendSequenceHeader(std::vector<uint8_t>& out, uint32_t width, uint32_t height) {
    appendStartCode(out, 0xB3);
    Bits bits;
    bits.put(width, 12);
    bits.put(height, 12);
    bits.put(1, 4);    // square pixels
    bits.put(3, 4);    // 25 fps
    bits.put(0x3FFFF, 18);
    bits.put(1, 1);    // marker_bit
    bits.put(0, 8);    // vbv_buffer_size
    bits.put(0, 1);    // constrained_parameters_flag
    bits.put(0, 1);    // no intra matrix
    bits.put(0, 1);    // no non-intra matrix
    appendBytes(out, bits.bytes());
}

// One macroblock of flat grey, so a decoded picture has a known value.
void appendFlatIntraPicture(std::vector<uint8_t>& out) {
    appendStartCode(out, 0x00);
    {
        Bits bits;
        bits.put(0, 10);       // temporal_reference
        bits.put(1, 3);        // picture_coding_type
        bits.put(0xFFFF, 16);  // vbv_delay
        bits.put(1, 1);        // extra_bit_picture
        appendBytes(out, bits.bytes());
    }
    appendStartCode(out, 0x01);
    {
        Bits bits;
        bits.put(16, 5);   // quantiser_scale_code
        bits.put(0, 1);    // intra_slice_flag
        bits.put(1, 1);    // extra_bit_slice
        bits.put(1, 1);    // skip_1stop_8data_bits
        bits.put(1, 1);    // macroblock_address_increment of 0
        bits.put(1, 1);    // macroblock_type: intra
        for (int block = 0; block < 6; ++block) {
            bits.put(4, 3);  // luma DC difference of 0
            bits.put(0, 2);  // chroma DC difference of 0
            bits.put(2, 2);  // end_of_block
        }
        appendBytes(out, bits.bytes());
    }
}

void appendSequenceEnd(std::vector<uint8_t>& out) { appendStartCode(out, 0xB7); }

// Two I pictures of 16x16 followed by the sequence end code.
std::vector<uint8_t> twoPictureStream() {
    std::vector<uint8_t> out;
    appendSequenceHeader(out, 16, 16);
    appendFlatIntraPicture(out);
    appendFlatIntraPicture(out);
    appendSequenceEnd(out);
    return out;
}

}  // namespace

// ---------------------------------------------------------------------------
// esds
// ---------------------------------------------------------------------------

TEST(Esds, ReadsTheVideoObjectType) {
    // version/flags; ES_Descriptor of length 22 = ES_ID(2) + flags(1) +
    // DecoderConfigDescriptor of length 17 = a 13 byte body plus a
    // DecoderSpecificInfo of length 2.
    std::vector<uint8_t> esds = {
        0x00, 0x00, 0x00, 0x00,
        0x03, 22,                           // ES_Descriptor
        0x00, 0x01,                         // ES_ID
        0x00,                               // flags
        0x04, 17,                           // DecoderConfigDescriptor
        kObjectTypeMpeg1Video,              // object_type_indication
        0x04,                               // stream_type: video
        0x00, 0x18, 0x00,                   // buffer_size
        0x00, 0x00, 0x00, 0x00,             // max_bitrate
        0x00, 0x00, 0x00, 0x00,             // avg_bitrate
        0x05, 2, 0x00, 0x00,                // DecoderSpecificInfo
    };
    ASSERT_EQ(esds.size(), 28u);

    EsdsInfo info;
    std::string error;
    ASSERT_TRUE(parseEsds(esds.data(), esds.size(), info, &error)) << error;
    EXPECT_EQ(info.object_type, kObjectTypeMpeg1Video);
    EXPECT_TRUE(info.valid);
}

TEST(Esds, RejectsGarbage) {
    const std::vector<uint8_t> esds = {0x01, 0x00, 0x00, 0x00, 0x04};
    EsdsInfo info;
    std::string error;
    EXPECT_FALSE(parseEsds(esds.data(), esds.size(), info, &error));
    EXPECT_FALSE(error.empty());

    EsdsInfo empty;
    EXPECT_FALSE(parseEsds(nullptr, 0, empty, nullptr));
}

TEST(Esds, DistinguishesVideoObjectTypes) {
    for (uint8_t type : {kObjectTypeMpeg4Visual, kObjectTypeAvc, kObjectTypeMpeg2SimpleVideo,
                         kObjectTypeMpeg2HighVideo, kObjectTypeMpeg2_422Video}) {
        std::vector<uint8_t> esds = {
            0x00, 0x00, 0x00, 0x00,
            0x03, 18,                  // ES_ID(2) + flags(1) + config(2 + 13)
            0x00, 0x01,
            0x00,
            0x04, 13,                  // DecoderConfigDescriptor
            type, 0x04, 0x00, 0x18, 0x00,
            0x00, 0x00, 0x00, 0x00,
            0x00, 0x00, 0x00, 0x00,
        };
        EsdsInfo info;
        std::string error;
        ASSERT_TRUE(parseEsds(esds.data(), esds.size(), info, &error)) << error;
        // The parser must report the object type verbatim so the demuxer can
        // tell MPEG-1, MPEG-2 and MPEG-4 apart.
        EXPECT_EQ(info.object_type, type);
    }
}

TEST(Esds, ClassifiesTheVideoObjectTypes) {
    EXPECT_FALSE(isMpeg2ObjectType(kObjectTypeMpeg1Video));
    EXPECT_FALSE(isMpeg2ObjectType(kObjectTypeMpeg4Visual));
    EXPECT_FALSE(isMpeg2ObjectType(kObjectTypeAvc));
    for (uint8_t type = kObjectTypeMpeg2SimpleVideo; type <= kObjectTypeMpeg2_422Video; ++type) {
        EXPECT_TRUE(isMpeg2ObjectType(type)) << "object type " << static_cast<int>(type);
    }
}

// ---------------------------------------------------------------------------
// RawMpegDemuxer
// ---------------------------------------------------------------------------

TEST(RawMpegDemuxer, CutsPicturesAtPictureHeaders) {
    const std::filesystem::path path = writeBytes(twoPictureStream(), "player_mpeg1_demux.mpg");

    {
        RawMpegDemuxer demuxer(25.0);
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        EXPECT_EQ(demuxer.video().codec, Codec::Mpeg1);
        EXPECT_EQ(demuxer.video().width, 16u);
        EXPECT_EQ(demuxer.video().height, 16u);
        EXPECT_TRUE(demuxer.video().format.annexb);
        EXPECT_DOUBLE_EQ(demuxer.duration(), 2.0 / 25.0);

        Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_TRUE(sample.keyframe);
        EXPECT_EQ(sample.pts, 0);
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_TRUE(sample.keyframe);
        EXPECT_EQ(sample.pts, 40);
        EXPECT_FALSE(demuxer.readSample(sample));
    }

    std::filesystem::remove(path);
}

TEST(RawMpegDemuxer, SampleDataStartsWithASequenceHeader) {
    const std::filesystem::path path = writeBytes(twoPictureStream(), "player_mpeg1_first.mpg");

    {
        RawMpegDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        // The first picture carries the sequence header, which the decoder
        // needs before it can decode anything.
        ASSERT_GE(sample.data.size(), 4u);
        EXPECT_EQ(sample.data[3], 0xB3);
    }

    std::filesystem::remove(path);
}

TEST(RawMpegDemuxer, SeekBacksUpToAnIntraPicture) {
    const std::filesystem::path path = writeBytes(twoPictureStream(), "player_mpeg1_seek.mpg");

    {
        RawMpegDemuxer demuxer(25.0);
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        ASSERT_TRUE(demuxer.seek(10.0));  // past the end clamps to the last picture
        Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 40);
    }

    std::filesystem::remove(path);
}

TEST(RawMpegDemuxer, RejectsAStreamWithoutPictures) {
    std::vector<uint8_t> stream;
    appendSequenceHeader(stream, 16, 16);
    appendSequenceEnd(stream);
    const std::filesystem::path path = writeBytes(stream, "player_mpeg1_empty.mpg");

    {
        RawMpegDemuxer demuxer;
        std::string error;
        EXPECT_FALSE(demuxer.open(path, error));
        EXPECT_NE(error.find("no MPEG-1 pictures"), std::string::npos);
    }

    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// container detection
// ---------------------------------------------------------------------------

TEST(RawMpegDemuxer, FactoryDetectsAnMpeg1Stream) {
    const std::filesystem::path path = writeBytes(twoPictureStream(), "player_mpeg1_factory.mpg");
    {
        std::string error;
        std::unique_ptr<IDemuxer> demuxer = createDemuxer(path, error);
        ASSERT_TRUE(demuxer) << error;
        EXPECT_EQ(demuxer->video().codec, Codec::Mpeg1);
    }
    std::filesystem::remove(path);
}

TEST(RawMpegDemuxer, FactoryStillDetectsRawHevc) {
    // An HEVC start code byte is always even and below 0x80, so 0x40 (VPS) must
    // not be mistaken for MPEG-1.
    std::vector<uint8_t> stream = {0x00, 0x00, 0x00, 0x01, 0x40, 0x01, 0x0C, 0x01};
    const std::filesystem::path path = writeBytes(stream, "player_mpeg1_not_mpeg1.hevc");
    {
        std::string error;
        // The stream is not decodable, but the demuxer that was chosen must be
        // the HEVC one, which reports its own error rather than an MPEG-1 one.
        std::unique_ptr<IDemuxer> demuxer = createDemuxer(path, error);
        EXPECT_TRUE(demuxer != nullptr) << error;
    }
    std::filesystem::remove(path);
}

// ---------------------------------------------------------------------------
// demuxer feeding the decoder
// ---------------------------------------------------------------------------

TEST(Mpeg1Pipeline, DecodesEveryPictureFromTheRawDemuxer) {
    const std::filesystem::path path = writeBytes(twoPictureStream(), "player_mpeg1_pipeline.mpg");

    std::vector<DecodedFrame> frames;
    {
        std::string error;
        std::unique_ptr<IDemuxer> demuxer = createDemuxer(path, error);
        ASSERT_TRUE(demuxer) << error;
        ASSERT_EQ(demuxer->video().codec, Codec::Mpeg1);

        mpeg12::Decoder decoder;
        Sample sample;
        while (demuxer->readSample(sample)) {
            decoder.pushSample(sample.data.data(), sample.data.size(), sample.pts);
            decoder.pullFrames(frames);
        }
        decoder.flush(frames);
    }

    // Two I pictures are both output; the second one only leaves the reference
    // set at flush.
    ASSERT_EQ(frames.size(), 2u);
    for (const DecodedFrame& frame : frames) {
        ASSERT_TRUE(frame.image.valid());
        EXPECT_EQ(frame.slice_type, SliceType::I);
        EXPECT_EQ(frame.image.width_, 16);
        EXPECT_EQ(frame.image.height_, 16);
        EXPECT_EQ(frame.image.plane(0)[0], 128);
    }

    std::filesystem::remove(path);
}


