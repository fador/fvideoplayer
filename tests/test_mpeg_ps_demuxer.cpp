// MPEG program stream demuxer tests.
//
// The test builds a program stream by hand: pack header, system header, and
// PES packets carrying an MPEG-2 elementary stream, then checks that the
// demuxer reassembles exactly that elementary stream and recovers the PTS.

#include "demux/Demuxer.hpp"
#include "demux/MpegPsDemuxer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace player::demux;

std::filesystem::path writeBytes(const std::vector<uint8_t>& data, const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path;
}

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

void putBe16(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

// A five byte PTS field: marker nibble, three bit groups split by marker bits.
void putPts(std::vector<uint8_t>& out, int64_t pts, bool with_dts) {
    const uint32_t p = static_cast<uint32_t>(pts);
    out.push_back(static_cast<uint8_t>((with_dts ? 0x30 : 0x20) | ((p >> 29) & 0x0E) | 1));
    putBe16(out, ((p >> 14) & 0xFFFE) | 1);
    putBe16(out, ((p << 1) & 0xFFFE) | 1);
}

// An MPEG-2 pack header with no stuffing. Ten bytes follow the start code; the
// last holds the reserved bits and the three bit stuffing count.
void appendPackHeader(std::vector<uint8_t>& out) {
    startCode(out, 0xBA);
    out.push_back(0x44);  // '01' + SCR[32..30] + marker
    out.push_back(0x00);  // SCR[29..22]
    out.push_back(0x04);  // SCR[21..15] + marker
    out.push_back(0x00);  // SCR[14..7]
    out.push_back(0x04);  // SCR[6..0] + marker
    out.push_back(0x00);  // SCR extension high bits
    out.push_back(0x04);  // SCR extension low bits + marker
    out.push_back(0x01);  // program_mux_rate[21..14]
    out.push_back(0x00);  // program_mux_rate[13..6]
    out.push_back(0xF8);  // mux_rate low bits, reserved, stuffing length 0
}

void appendSystemHeader(std::vector<uint8_t>& out) {
    startCode(out, 0xBB);
    putBe16(out, 6);      // header length
    out.push_back(0x80);  // marker
    out.push_back(0x00);  // rate bound
    out.push_back(0x00);
    out.push_back(0x01);  // one video stream
    out.push_back(0xE0);
    out.push_back(0x00);
}

// An MPEG-2 PES packet carrying `payload` on video stream 0xE0.
void appendPes(std::vector<uint8_t>& out, int64_t pts, const std::vector<uint8_t>& payload) {
    startCode(out, 0xE0);
    const size_t header_length = 5;  // just the PTS
    putBe16(out, static_cast<uint32_t>(3 + header_length + payload.size()));
    out.push_back(0x80);  // '10', no scrambling, no priority, aligned
    out.push_back(0x80);  // PTS only
    out.push_back(static_cast<uint8_t>(header_length));
    putPts(out, pts, false);
    append(out, payload);
}

// An MPEG-2 elementary stream holding one I picture with `slices` slices.
std::vector<uint8_t> buildElementaryPicture(int temporal_reference, int slices) {
    std::vector<uint8_t> out;

    startCode(out, 0xB3);
    {
        Bits bits;
        bits.put(16, 12);
        bits.put(16, 12);
        bits.put(1, 4);
        bits.put(3, 4);
        bits.put(0x3FFFF, 18);
        bits.put(1, 1);
        bits.put(0, 10);
        bits.put(0, 1);
        bits.put(0, 1);
        bits.put(0, 1);
        append(out, bits.bytes());
    }
    startCode(out, 0xB5);
    {
        Bits bits;
        bits.put(1, 4);
        bits.put(0x48, 8);
        bits.put(1, 1);
        bits.put(1, 2);
        bits.put(0, 2);
        bits.put(0, 2);
        bits.put(0, 12);
        bits.put(1, 1);
        bits.put(0, 8);
        bits.put(0, 1);
        bits.put(0, 2);
        bits.put(0, 5);
        append(out, bits.bytes());
    }
    startCode(out, 0x00);
    {
        Bits bits;
        bits.put(static_cast<uint32_t>(temporal_reference), 10);
        bits.put(1, 3);
        bits.put(0xFFFF, 16);
        bits.put(0, 1);
        append(out, bits.bytes());
    }
    startCode(out, 0xB5);
    {
        Bits bits;
        bits.put(8, 4);
        for (int i = 0; i < 4; ++i) {
            bits.put(0xF, 4);
        }
        bits.put(0, 2);  // intra_dc_precision
        bits.put(3, 2);  // frame picture
        bits.put(0, 1);
        bits.put(1, 1);
        bits.put(0, 1);
        bits.put(0, 1);
        bits.put(0, 1);
        bits.put(0, 1);
        bits.put(0, 1);
        bits.put(0, 1);
        bits.put(1, 1);
        bits.put(0, 1);
        append(out, bits.bytes());
    }
    for (int slice = 0; slice < slices; ++slice) {
        startCode(out, static_cast<uint8_t>(0x01 + slice));
        Bits bits;
        bits.put(8, 5);
        bits.put(0, 1);  // skip_1stop_8data_bits stop bit
        bits.put(1, 1);  // address increment of 0
        bits.put(1, 1);  // intra
        for (int block = 0; block < 6; ++block) {
            if (block == 0 || block >= 4) {
                // DC difference of +8 so the picture cannot be confused with
                // the neutral fill, and so every intra_dc_precision lands on a
                // distinguishable value. A difference is DPCM and Cb and Cr
                // have separate predictors, so the first luma block and both
                // chroma blocks carry it.
                if (block == 0) {
                    bits.putCode(0xe, 4);    // luma DC symbol 5
                    bits.put(0b01000, 5);    // +8
                } else {
                    bits.putCode(0x1e, 5);   // chroma DC symbol 5
                    bits.put(0b01000, 5);    // +8
                }
            } else {
                bits.putCode(0x4, 3);        // luma DC difference of 0
            }
            bits.putCode(0x2, 2);            // end_of_block
        }
        append(out, bits.bytes());
    }
    return out;
}

// Two pictures, each in its own PES packet, with a PTS on each.
std::vector<uint8_t> buildProgramStream() {
    std::vector<uint8_t> out;
    appendPackHeader(out);
    appendSystemHeader(out);
    appendPes(out, 90000 * 1, buildElementaryPicture(0, 1));   // 1 s
    appendPes(out, 90000 * 2, buildElementaryPicture(1, 1));   // 2 s
    startCode(out, 0xB9);  // program end
    return out;
}

}  // namespace

TEST(MpegPsDemuxer, ReadsTwoPicturesAndTheirTimestamps) {
    const std::filesystem::path path = writeBytes(buildProgramStream(), "player_ps_basic.mpg");
    {
        MpegPsDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        EXPECT_EQ(demuxer.video().codec, Codec::Mpeg2);
        EXPECT_EQ(demuxer.video().width, 16u);
        EXPECT_EQ(demuxer.video().height, 16u);
        EXPECT_TRUE(demuxer.video().format.annexb);

        Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 1000);
        EXPECT_TRUE(sample.keyframe);
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 2000);
        EXPECT_FALSE(demuxer.readSample(sample));
    }
    std::filesystem::remove(path);
}

TEST(MpegPsDemuxer, ReassemblesPicturesFromTheirPesPackets) {
    const std::filesystem::path path = writeBytes(buildProgramStream(), "player_ps_es.mpg");
    {
        MpegPsDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        // The first sample must carry the sequence header so the decoder can
        // configure itself, and it must end before the next picture header.
        Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        ASSERT_GE(sample.data.size(), 4u);
        EXPECT_EQ(sample.data[3], 0xB3);
        // Exactly one picture header in the sample.
        size_t picture_headers = 0;
        for (size_t i = 0; i + 3 < sample.data.size(); ++i) {
            if (sample.data[i] == 0 && sample.data[i + 1] == 0 && sample.data[i + 2] == 1 &&
                sample.data[i + 3] == 0x00) {
                ++picture_headers;
            }
        }
        EXPECT_EQ(picture_headers, 1u);
    }
    std::filesystem::remove(path);
}

TEST(MpegPsDemuxer, DerivesTheFrameRateFromTheTimestamps) {
    // The two PTS values are one second apart, so the frame rate is 1 fps.
    const std::filesystem::path path = writeBytes(buildProgramStream(), "player_ps_fps.mpg");
    {
        MpegPsDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;
        EXPECT_DOUBLE_EQ(demuxer.video().fps, 1.0);
        EXPECT_DOUBLE_EQ(demuxer.duration(), 2.0);
    }
    std::filesystem::remove(path);
}

TEST(MpegPsDemuxer, SeekBacksUpToAnIntraPicture) {
    const std::filesystem::path path = writeBytes(buildProgramStream(), "player_ps_seek.mpg");
    {
        MpegPsDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        ASSERT_TRUE(demuxer.seek(10.0));  // past the end clamps
        Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 2000);
    }
    std::filesystem::remove(path);
}

TEST(MpegPsDemuxer, SplitsAPictureAcrossPesPackets) {
    // The same two pictures, but the first PES packet is cut in half so the
    // picture has to be reassembled from two packets.
    const std::vector<uint8_t> first = buildElementaryPicture(0, 1);
    const size_t cut = first.size() / 2;

    std::vector<uint8_t> stream;
    appendPackHeader(stream);
    appendSystemHeader(stream);
    appendPes(stream, 90000, std::vector<uint8_t>(first.begin(), first.begin() + cut));
    appendPes(stream, 90000, std::vector<uint8_t>(first.begin() + cut, first.end()));
    appendPes(stream, 90000 * 2, buildElementaryPicture(1, 1));
    startCode(stream, 0xB9);

    const std::filesystem::path path = writeBytes(stream, "player_ps_split.mpg");
    {
        MpegPsDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        ASSERT_EQ(sample.data.size(), first.size()) << "the split picture was not reassembled";
        EXPECT_EQ(sample.data, first) << "the split picture was not reassembled byte for byte";
        EXPECT_EQ(sample.pts, 1000);
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 2000);
    }
    std::filesystem::remove(path);
}

TEST(MpegPsDemuxer, FactoryDetectsAProgramStream) {
    const std::filesystem::path path = writeBytes(buildProgramStream(), "player_ps_factory.mpg");
    {
        std::string error;
        std::unique_ptr<IDemuxer> demuxer = createDemuxer(path, error);
        ASSERT_TRUE(demuxer) << error;
        EXPECT_EQ(demuxer->video().codec, Codec::Mpeg2);
    }
    std::filesystem::remove(path);
}

TEST(MpegPsDemuxer, RejectsAStreamWithoutAPackHeader) {
    // A bare elementary stream must not be mistaken for a program stream.
    std::vector<uint8_t> stream;
    appendPes(stream, 90000, buildElementaryPicture(0, 1));
    const std::filesystem::path path = writeBytes(stream, "player_ps_nopack.mpg");

    MpegPsDemuxer demuxer;
    std::string error;
    EXPECT_FALSE(demuxer.open(path, error));
    EXPECT_NE(error.find("no pack header"), std::string::npos);

    std::filesystem::remove(path);
}

TEST(MpegPsDemuxer, Mpeg1PesHeaderIsParsed) {
    // An MPEG-1 program stream uses an `01` marker with a STD buffer field
    // before the PTS, rather than the MPEG-2 flags and header length.
    std::vector<uint8_t> stream;
    startCode(stream, 0xBA);
    for (int i = 0; i < 8; ++i) {
        stream.push_back(0x20);  // '0010' marker and clock fields
    }
    const std::vector<uint8_t> picture = buildElementaryPicture(0, 1);
    startCode(stream, 0xE0);
    putBe16(stream, static_cast<uint32_t>(2 + 5 + picture.size()));
    stream.push_back(0x40);  // '01' + STD buffer scale
    stream.push_back(0x00);  // STD buffer size
    putPts(stream, 90000 * 3, false);
    append(stream, picture);
    startCode(stream, 0xB9);

    const std::filesystem::path path = writeBytes(stream, "player_ps_mpeg1.mpg");
    {
        MpegPsDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;
        // The elementary stream itself carries a sequence extension, so the
        // video is MPEG-2 even though the PES header is the MPEG-1 form.
        EXPECT_EQ(demuxer.video().codec, Codec::Mpeg2);
        Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 3000);
    }
    std::filesystem::remove(path);
}
