#include "demux/Demuxer.hpp"
#include "demux/RawHevcDemuxer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using player::demux::Codec;
using player::demux::RawHevcDemuxer;
using player::demux::Sample;

std::vector<uint8_t> nal(uint8_t type, std::initializer_list<uint8_t> payload, bool first_slice = true) {
    std::vector<uint8_t> out = {0, 0, 0, 1, static_cast<uint8_t>(type << 1), 0x01};
    if (type <= 31) {
        out.push_back(first_slice ? 0x80 : 0x00);
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

void append(std::vector<uint8_t>& out, const std::vector<uint8_t>& data) {
    out.insert(out.end(), data.begin(), data.end());
}

std::filesystem::path writeStream(const std::vector<uint8_t>& data, const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path;
}

std::vector<uint8_t> basicStream() {
    std::vector<uint8_t> stream;
    append(stream, nal(32, {0x0C, 0x01}));  // VPS
    append(stream, nal(33, {0x42, 0x01}));  // SPS
    append(stream, nal(34, {0x44, 0x01}));  // PPS
    append(stream, nal(19, {0xAA, 0xBB}));  // IDR_W_RADL
    append(stream, nal(1, {0xCC}));         // TRAIL_R
    append(stream, nal(1, {0xDD}));         // TRAIL_R
    return stream;
}

}  // namespace

TEST(RawHevcDemuxer, ParsesAccessUnitsAndKeyframes) {
    const std::filesystem::path path = writeStream(basicStream(), "hevc_player_raw_basic.hevc");
    {
        RawHevcDemuxer demuxer(30.0);
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        EXPECT_EQ(demuxer.video().codec, Codec::Hevc);
        EXPECT_TRUE(demuxer.video().format.annexb);
        EXPECT_DOUBLE_EQ(demuxer.video().fps, 30.0);
        EXPECT_NEAR(demuxer.duration(), 0.1, 1e-9);

        Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_TRUE(sample.keyframe);
        EXPECT_EQ(sample.pts, 0);
        EXPECT_GT(sample.data.size(), static_cast<size_t>(20));

        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_FALSE(sample.keyframe);
        EXPECT_EQ(sample.pts, 33);

        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_FALSE(sample.keyframe);
        EXPECT_EQ(sample.pts, 67);

        EXPECT_FALSE(demuxer.readSample(sample));

        ASSERT_TRUE(demuxer.seek(0.0));
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_TRUE(sample.keyframe);
        EXPECT_EQ(sample.pts, 0);
    }
    std::filesystem::remove(path);
}

TEST(RawHevcDemuxer, MultiSliceStaysInOneAccessUnit) {
    std::vector<uint8_t> stream;
    append(stream, nal(19, {0xAA}, true));   // first slice
    append(stream, nal(19, {0xBB}, false));  // second slice of same picture

    const std::filesystem::path path = writeStream(stream, "hevc_player_raw_slices.hevc");
    {
        RawHevcDemuxer demuxer(25.0);
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_TRUE(sample.keyframe);
        EXPECT_FALSE(demuxer.readSample(sample));
    }
    std::filesystem::remove(path);
}

TEST(RawHevcDemuxer, SeekFindsPreviousKeyframe) {
    std::vector<uint8_t> stream;
    append(stream, nal(19, {0x01}));  // IDR at index 0
    for (int i = 0; i < 9; ++i) {
        append(stream, nal(1, {static_cast<uint8_t>(i)}));  // 9 non-key frames
    }
    append(stream, nal(19, {0x02}));  // IDR at index 10
    for (int i = 0; i < 5; ++i) {
        append(stream, nal(1, {static_cast<uint8_t>(i)}));
    }

    const std::filesystem::path path = writeStream(stream, "hevc_player_raw_seek.hevc");
    {
        RawHevcDemuxer demuxer(10.0);
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        ASSERT_TRUE(demuxer.seek(1.2));  // target index 12 -> previous keyframe index 10
        Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_TRUE(sample.keyframe);
        EXPECT_EQ(sample.pts, 1000);
    }
    std::filesystem::remove(path);
}

TEST(RawHevcDemuxer, FactoryDetectsAnnexB) {
    const std::filesystem::path path = writeStream(basicStream(), "hevc_player_raw_factory.hevc");
    {
        std::string error;
        std::unique_ptr<player::demux::IDemuxer> demuxer = player::demux::createDemuxer(path, error);
        ASSERT_TRUE(demuxer) << error;
        EXPECT_EQ(demuxer->video().codec, Codec::Hevc);
    }
    std::filesystem::remove(path);
}
