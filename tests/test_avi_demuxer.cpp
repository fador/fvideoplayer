#include "demux/AviDemuxer.hpp"
#include "demux/Demuxer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

constexpr uint32_t kFourcc(const char* tag) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(tag[0])) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(tag[1])) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(tag[2])) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(tag[3]));
}

void putU16(Bytes& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void putU32(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void append(Bytes& out, const Bytes& data) {
    out.insert(out.end(), data.begin(), data.end());
}

Bytes chunk(uint32_t id, const Bytes& payload) {
    Bytes out;
    putU32(out, id);
    putU32(out, static_cast<uint32_t>(payload.size()));
    append(out, payload);
    if (payload.size() & 1u) {
        out.push_back(0);
    }
    return out;
}

Bytes listChunk(const char* type, const Bytes& payload) {
    Bytes body;
    putU32(body, kFourcc(type));
    append(body, payload);
    return chunk(kFourcc("LIST"), body);
}

Bytes makeHvcc() {
    Bytes out;
    out.push_back(1);
    out.push_back(2);
    putU32(out, 0x60000000);
    for (int i = 0; i < 6; ++i) {
        out.push_back(0);
    }
    out.push_back(120);
    putU16(out, 0xF000);
    out.push_back(0xFC);
    out.push_back(0xFD);
    out.push_back(0xFA);
    out.push_back(0xF8);
    putU16(out, 0);
    out.push_back(0x0F);
    out.push_back(1);
    out.push_back(0xA0);
    putU16(out, 1);
    putU16(out, 2);
    out.push_back(0x40);
    out.push_back(0x01);
    return out;
}

Bytes avih() {
    Bytes payload;
    putU32(payload, 33333);  // dwMicroSecPerFrame
    putU32(payload, 0);      // dwMaxBytesPerSec
    putU32(payload, 0);      // dwPaddingGranularity
    putU32(payload, 0x10);   // dwFlags
    putU32(payload, 4);      // dwTotalFrames
    putU32(payload, 0);      // dwInitialFrames
    putU32(payload, 1);      // dwStreams
    putU32(payload, 0);      // dwSuggestedBufferSize
    putU32(payload, 1280);   // dwWidth
    putU32(payload, 720);    // dwHeight
    for (int i = 0; i < 4; ++i) {
        putU32(payload, 0);
    }
    return chunk(kFourcc("avih"), payload);
}

Bytes strh() {
    Bytes payload;
    putU32(payload, kFourcc("vids"));
    putU32(payload, kFourcc("HEVC"));
    putU32(payload, 0);  // dwFlags
    putU16(payload, 0);  // wPriority
    putU16(payload, 0);  // wLanguage
    putU32(payload, 0);  // dwInitialFrames
    putU32(payload, 1);  // dwScale
    putU32(payload, 30);  // dwRate
    putU32(payload, 0);  // dwStart
    putU32(payload, 4);  // dwLength
    putU32(payload, 0);  // dwSuggestedBufferSize
    putU32(payload, 0);  // dwQuality
    putU32(payload, 0);  // dwSampleSize
    putU32(payload, 0);  // rcFrame left/top
    putU32(payload, 0);  // rcFrame right/bottom
    return chunk(kFourcc("strh"), payload);
}

Bytes strfVideo(bool annexb) {
    Bytes payload;
    putU32(payload, 40);  // biSize
    putU32(payload, 1280);
    putU32(payload, 720);
    putU16(payload, 1);   // biPlanes
    putU16(payload, 24);  // biBitCount
    putU32(payload, kFourcc("HEVC"));
    putU32(payload, 0);
    putU32(payload, 0);
    putU32(payload, 0);
    putU32(payload, 0);
    putU32(payload, 0);
    if (annexb) {
        const Bytes extradata = {0, 0, 0, 1, 0x40, 0x01, 0x0C};
        append(payload, extradata);
    } else {
        append(payload, makeHvcc());
    }
    return chunk(kFourcc("strf"), payload);
}

Bytes sampleData(size_t index, bool annexb) {
    const bool keyframe = index == 0 || index == 2;
    const uint8_t nal_type = keyframe ? 0x26 : 0x02;
    const uint8_t payload = static_cast<uint8_t>(0xAA + index);
    if (annexb) {
        return {0, 0, 0, 1, nal_type, 0x01, payload};
    }
    Bytes out;
    putU32(out, 3);  // NAL length
    out.push_back(nal_type);
    out.push_back(0x01);
    out.push_back(payload);
    return out;
}

Bytes buildAvi(bool with_idx1, bool annexb) {
    Bytes hdrl_payload = avih();
    Bytes strl_payload = strh();
    append(strl_payload, strfVideo(annexb));
    append(hdrl_payload, listChunk("strl", strl_payload));
    const Bytes hdrl = listChunk("hdrl", hdrl_payload);

    Bytes movi_data;
    std::vector<uint32_t> offsets;
    std::vector<uint32_t> sizes;
    std::vector<bool> keyframes;
    for (size_t i = 0; i < 4; ++i) {
        const Bytes data = sampleData(i, annexb);
        offsets.push_back(static_cast<uint32_t>(movi_data.size()));
        sizes.push_back(static_cast<uint32_t>(data.size()));
        keyframes.push_back(i == 0 || i == 2);
        append(movi_data, chunk(kFourcc("00dc"), data));
    }
    const Bytes movi = listChunk("movi", movi_data);

    Bytes file;
    putU32(file, kFourcc("RIFF"));
    Bytes riff_body;
    putU32(riff_body, kFourcc("AVI "));
    append(riff_body, hdrl);
    append(riff_body, movi);
    if (with_idx1) {
        Bytes entries;
        for (size_t i = 0; i < offsets.size(); ++i) {
            putU32(entries, kFourcc("00dc"));
            putU32(entries, keyframes[i] ? 0x10 : 0);
            putU32(entries, offsets[i]);
            putU32(entries, sizes[i]);
        }
        append(riff_body, chunk(kFourcc("idx1"), entries));
    }
    putU32(file, static_cast<uint32_t>(riff_body.size()));
    append(file, riff_body);
    return file;
}

std::filesystem::path writeAvi(const Bytes& data, const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path;
}

}  // namespace

TEST(AviDemuxer, ParsesVideoTrackAndSamples) {
    const std::filesystem::path path = writeAvi(buildAvi(true, false), "hevc_player_avi_basic.avi");
    {
        player::demux::AviDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        EXPECT_EQ(demuxer.video().codec, player::demux::Codec::Hevc);
        EXPECT_EQ(demuxer.video().width, 1280u);
        EXPECT_EQ(demuxer.video().height, 720u);
        EXPECT_DOUBLE_EQ(demuxer.video().fps, 30.0);
        EXPECT_EQ(demuxer.video().timescale, 1000u);
        EXPECT_FALSE(demuxer.video().format.annexb);
        EXPECT_EQ(demuxer.video().format.nal_length_size, 4);
        EXPECT_FALSE(demuxer.video().hvcc.empty());
        EXPECT_NEAR(demuxer.duration(), 4.0 / 30.0, 1e-9);

        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 0);
        EXPECT_TRUE(sample.keyframe);
        EXPECT_EQ(sample.data.size(), 7u);
        EXPECT_EQ(sample.data[3], 3u);

        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 33);
        EXPECT_FALSE(sample.keyframe);

        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 67);
        EXPECT_TRUE(sample.keyframe);

        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 100);
        EXPECT_FALSE(sample.keyframe);

        EXPECT_FALSE(demuxer.readSample(sample));
    }
    std::filesystem::remove(path);
}

TEST(AviDemuxer, ScansMoviWithoutIdx1) {
    const std::filesystem::path path = writeAvi(buildAvi(false, false), "hevc_player_avi_scan.avi");
    {
        player::demux::AviDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_TRUE(sample.keyframe);
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_FALSE(sample.keyframe);
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_TRUE(sample.keyframe);
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_FALSE(sample.keyframe);
        EXPECT_FALSE(demuxer.readSample(sample));
    }
    std::filesystem::remove(path);
}

TEST(AviDemuxer, DetectsAnnexBStream) {
    const std::filesystem::path path = writeAvi(buildAvi(true, true), "hevc_player_avi_annexb.avi");
    {
        player::demux::AviDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        EXPECT_TRUE(demuxer.video().format.annexb);
        EXPECT_TRUE(demuxer.video().hvcc.empty());

        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_TRUE(sample.keyframe);
        // Annex B parameter sets are prepended to keyframes.
        EXPECT_EQ(sample.data.size(), 7u + 7u);
        EXPECT_EQ(sample.data[0], 0u);
        EXPECT_EQ(sample.data[3], 1u);

        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_FALSE(sample.keyframe);
        EXPECT_EQ(sample.data.size(), 7u);
    }
    std::filesystem::remove(path);
}

TEST(AviDemuxer, SeeksToPreviousKeyframe) {
    const std::filesystem::path path = writeAvi(buildAvi(true, false), "hevc_player_avi_seek.avi");
    {
        player::demux::AviDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        ASSERT_TRUE(demuxer.seek(0.08));  // 80 ms -> previous keyframe at 67
        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 67);
        EXPECT_TRUE(sample.keyframe);
    }
    std::filesystem::remove(path);
}

TEST(AviDemuxer, FactoryDetectsAvi) {
    const std::filesystem::path path = writeAvi(buildAvi(true, false), "hevc_player_avi_factory.avi");
    {
        std::string error;
        std::unique_ptr<player::demux::IDemuxer> demuxer = player::demux::createDemuxer(path, error);
        ASSERT_TRUE(demuxer) << error;
        EXPECT_EQ(demuxer->video().codec, player::demux::Codec::Hevc);
    }
    std::filesystem::remove(path);
}
