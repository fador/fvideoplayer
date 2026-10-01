#include "demux/Demuxer.hpp"
#include "demux/MkvDemuxer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

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

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& part : parts) {
        append(out, part);
    }
    return out;
}

void putEbmlId(Bytes& out, uint32_t id) {
    if (id <= 0xFF) {
        out.push_back(static_cast<uint8_t>(id));
    } else if (id <= 0xFFFF) {
        out.push_back(static_cast<uint8_t>(id >> 8));
        out.push_back(static_cast<uint8_t>(id & 0xFF));
    } else if (id <= 0xFFFFFF) {
        out.push_back(static_cast<uint8_t>(id >> 16));
        out.push_back(static_cast<uint8_t>((id >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(id & 0xFF));
    } else {
        putU32(out, id);
    }
}

void putEbmlVint(Bytes& out, uint64_t value) {
    int length = 1;
    while (length < 8 && value >= ((1ULL << (7 * length)) - 1)) {
        ++length;
    }
    const uint64_t encoded = value | (1ULL << (7 * length));
    for (int i = length - 1; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((encoded >> (8 * i)) & 0xFF));
    }
}

Bytes element(uint32_t id, const Bytes& payload) {
    Bytes out;
    putEbmlId(out, id);
    putEbmlVint(out, payload.size());
    append(out, payload);
    return out;
}

Bytes uintPayload(uint64_t value) {
    Bytes out;
    int length = 1;
    while (length < 8 && (value >> (8 * length)) != 0) {
        ++length;
    }
    for (int i = length - 1; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
    }
    return out;
}

Bytes uintElement(uint32_t id, uint64_t value) {
    return element(id, uintPayload(value));
}

Bytes floatElement(uint32_t id, double value) {
    Bytes payload(8);
    std::memcpy(payload.data(), &value, 8);
    return element(id, payload);
}

Bytes stringElement(uint32_t id, const std::string& value) {
    return element(id, Bytes(value.begin(), value.end()));
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
    out.push_back(0xA0);  // VPS array, 1 NAL
    putU16(out, 1);
    putU16(out, 2);
    out.push_back(0x40);
    out.push_back(0x01);
    return out;
}

Bytes frame(uint8_t nal_type, uint8_t second_byte, uint8_t payload) {
    Bytes nal = {nal_type, second_byte, payload};
    Bytes out;
    putU32(out, static_cast<uint32_t>(nal.size()));
    append(out, nal);
    return out;
}

Bytes simpleBlock(uint64_t track, int16_t relative, bool keyframe, const Bytes& block_data) {
    Bytes out;
    putEbmlVint(out, track);
    putU16(out, static_cast<uint16_t>(relative));
    out.push_back(keyframe ? 0x80 : 0x00);
    append(out, block_data);
    return out;
}

Bytes videoTrackEntry() {
    Bytes video;
    append(video, uintElement(0xB0, 1280));  // PixelWidth
    append(video, uintElement(0xBA, 720));   // PixelHeight

    Bytes children;
    append(children, uintElement(0xD7, 1));  // TrackNumber
    append(children, uintElement(0x83, 1));  // TrackType = video
    append(children, stringElement(0x86, "V_MPEGH/ISO/HEVC"));
    append(children, element(0x63A2, makeHvcc()));  // CodecPrivate
    append(children, uintElement(0x23E383, 33333333));  // DefaultDuration (30 fps)
    append(children, element(0xE0, video));
    return element(0xAE, children);
}

Bytes audioTrackEntry() {
    Bytes audio;
    append(audio, floatElement(0xB5, 48000.0));  // SamplingFrequency
    append(audio, uintElement(0x9F, 2));         // Channels

    Bytes children;
    append(children, uintElement(0xD7, 2));
    append(children, uintElement(0x83, 2));  // TrackType = audio
    append(children, stringElement(0x86, "A_AAC"));
    append(children, element(0xE1, audio));
    return element(0xAE, children);
}

Bytes buildMkv(bool with_audio) {
    Bytes ebml_header = element(0x1A45DFA3,
                                concat({uintElement(0x4286, 1), uintElement(0x42F7, 1),
                                        uintElement(0x42F2, 4), uintElement(0x42F3, 8),
                                        stringElement(0x4282, "matroska"),
                                        uintElement(0x4287, 4), uintElement(0x4285, 2)}));
    Bytes info = element(0x1549A966,
                         concat({uintElement(0x2AD7B1, 1000000), floatElement(0x4489, 133.3333)}));
    Bytes tracks_payload = videoTrackEntry();
    if (with_audio) {
        append(tracks_payload, audioTrackEntry());
    }
    Bytes tracks = element(0x1654AE6B, tracks_payload);
    Bytes cluster0 = element(0x1F43B675,
                             concat({uintElement(0xE7, 0),
                                     element(0xA3, simpleBlock(1, 0, true, frame(0x26, 0x01, 0xAA))),
                                     element(0xA3, simpleBlock(1, 33, false, frame(0x02, 0x01, 0xBB)))}));
    Bytes cluster1 = element(0x1F43B675,
                             concat({uintElement(0xE7, 67),
                                     element(0xA3, simpleBlock(1, 0, true, frame(0x26, 0x01, 0xCC))),
                                     element(0xA3, simpleBlock(1, 33, false, frame(0x02, 0x01, 0xDD)))}));
    Bytes segment = element(0x18538067, concat({info, tracks, cluster0, cluster1}));
    return concat({ebml_header, segment});
}

std::filesystem::path writeMkv(const Bytes& data, const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path;
}

}  // namespace

TEST(MkvDemuxer, ParsesVideoAndAudioTrack) {
    const std::filesystem::path path = writeMkv(buildMkv(true), "hevc_player_mkv_basic.mkv");
    {
        player::demux::MkvDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        EXPECT_EQ(demuxer.video().codec, player::demux::Codec::Hevc);
        EXPECT_EQ(demuxer.video().width, 1280u);
        EXPECT_EQ(demuxer.video().height, 720u);
        EXPECT_EQ(demuxer.video().timescale, 1000u);
        EXPECT_FALSE(demuxer.video().format.annexb);
        EXPECT_EQ(demuxer.video().format.nal_length_size, 4);
        EXPECT_FALSE(demuxer.video().hvcc.empty());
        EXPECT_NEAR(demuxer.video().fps, 30.0, 0.01);
        EXPECT_NEAR(demuxer.duration(), 0.1333333, 1e-6);

        EXPECT_TRUE(demuxer.audio().present);
        EXPECT_EQ(demuxer.audio().codec_fourcc, 0x6D703461u);  // 'mp4a'
        EXPECT_EQ(demuxer.audio().channels, 2u);
        EXPECT_EQ(demuxer.audio().sample_rate, 48000u);
    }
    std::filesystem::remove(path);
}

TEST(MkvDemuxer, ReadsSamplesWithPtsAndKeyframes) {
    const std::filesystem::path path = writeMkv(buildMkv(false), "hevc_player_mkv_samples.mkv");
    {
        player::demux::MkvDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

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

TEST(MkvDemuxer, SeeksToPreviousKeyframe) {
    const std::filesystem::path path = writeMkv(buildMkv(false), "hevc_player_mkv_seek.mkv");
    {
        player::demux::MkvDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        ASSERT_TRUE(demuxer.seek(0.08));  // 80 ms -> previous keyframe at 67
        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 67);
        EXPECT_TRUE(sample.keyframe);

        ASSERT_TRUE(demuxer.seek(0.05));  // 50 ms -> previous keyframe at 0
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 0);
    }
    std::filesystem::remove(path);
}

TEST(MkvDemuxer, FactoryDetectsMkv) {
    const std::filesystem::path path = writeMkv(buildMkv(false), "hevc_player_mkv_factory.mkv");
    {
        std::string error;
        std::unique_ptr<player::demux::IDemuxer> demuxer = player::demux::createDemuxer(path, error);
        ASSERT_TRUE(demuxer) << error;
        EXPECT_EQ(demuxer->video().codec, player::demux::Codec::Hevc);
    }
    std::filesystem::remove(path);
}
