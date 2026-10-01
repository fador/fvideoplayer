#include "demux/Demuxer.hpp"
#include "demux/Mp4Demuxer.hpp"

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

void putU64(Bytes& out, uint64_t value) {
    putU32(out, static_cast<uint32_t>(value >> 32));
    putU32(out, static_cast<uint32_t>(value & 0xFFFFFFFFu));
}

void append(Bytes& out, const Bytes& data) {
    out.insert(out.end(), data.begin(), data.end());
}

Bytes box(uint32_t type, const Bytes& payload) {
    Bytes out;
    putU32(out, static_cast<uint32_t>(payload.size() + 8));
    putU32(out, type);
    append(out, payload);
    return out;
}

Bytes fullBox(uint32_t type, uint8_t version, uint32_t flags, const Bytes& payload) {
    Bytes body;
    body.push_back(version);
    body.push_back(static_cast<uint8_t>((flags >> 16) & 0xFF));
    body.push_back(static_cast<uint8_t>((flags >> 8) & 0xFF));
    body.push_back(static_cast<uint8_t>(flags & 0xFF));
    append(body, payload);
    return box(type, body);
}

Bytes makeHvcc() {
    Bytes out;
    out.push_back(1);        // configurationVersion
    out.push_back(2);        // Main 10
    putU32(out, 0x60000000);  // compatibility flags
    for (int i = 0; i < 6; ++i) {
        out.push_back(0);
    }
    out.push_back(120);      // level 4.0
    putU16(out, 0xF000);     // min_spatial_segmentation_idc
    out.push_back(0xFC);     // parallelismType
    out.push_back(0xFD);     // chroma_format_idc 1
    out.push_back(0xFA);     // bit_depth_luma_minus8 = 2
    out.push_back(0xF8);
    putU16(out, 0);          // avgFrameRate
    out.push_back(0x0F);     // temporalIdNested, lengthSizeMinusOne 3
    out.push_back(3);        // numOfArrays
    const uint8_t types[3] = {32, 33, 34};
    const Bytes nals[3] = {{0x40, 0x01, 0x0C}, {0x42, 0x01, 0x02}, {0x44, 0x01, 0x03}};
    for (int i = 0; i < 3; ++i) {
        out.push_back(static_cast<uint8_t>(0x80 | types[i]));
        putU16(out, 1);
        putU16(out, static_cast<uint16_t>(nals[i].size()));
        append(out, nals[i]);
    }
    return out;
}

Bytes visualSampleEntry() {
    Bytes payload;
    for (int i = 0; i < 6; ++i) {
        payload.push_back(0);
    }
    putU16(payload, 1);  // data_reference_index
    for (int i = 0; i < 16; ++i) {
        payload.push_back(0);
    }
    putU16(payload, 1280);
    putU16(payload, 720);
    putU32(payload, 0x00480000);
    putU32(payload, 0x00480000);
    putU32(payload, 0);
    putU16(payload, 1);
    for (int i = 0; i < 32; ++i) {
        payload.push_back(0);
    }
    putU16(payload, 0x0018);
    putU16(payload, 0xFFFF);
    append(payload, box(kFourcc("hvcC"), makeHvcc()));
    return box(kFourcc("hvc1"), payload);
}

Bytes audioSampleEntry() {
    Bytes payload;
    for (int i = 0; i < 6; ++i) {
        payload.push_back(0);
    }
    putU16(payload, 1);  // data_reference_index
    putU16(payload, 0);  // version
    putU16(payload, 0);  // revision
    putU32(payload, 0);  // vendor
    putU16(payload, 2);  // channelcount
    putU16(payload, 16);  // samplesize
    putU16(payload, 0);
    putU16(payload, 0);
    putU32(payload, 44100u << 16);
    return box(kFourcc("mp4a"), payload);
}

Bytes tkhd(uint32_t track_id, uint32_t duration, uint32_t width, uint32_t height) {
    Bytes payload;
    putU32(payload, 0);          // creation
    putU32(payload, 0);          // modification
    putU32(payload, track_id);
    putU32(payload, 0);          // reserved
    putU32(payload, duration);
    for (int i = 0; i < 8; ++i) {
        payload.push_back(0);  // reserved
    }
    putU16(payload, 0);  // layer
    putU16(payload, 0);  // alternate_group
    putU16(payload, 0);  // volume
    putU16(payload, 0);  // reserved
    const uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (uint32_t value : matrix) {
        putU32(payload, value);
    }
    putU32(payload, width << 16);
    putU32(payload, height << 16);
    return fullBox(kFourcc("tkhd"), 0, 0x000007, payload);
}

Bytes mdhd(uint32_t timescale, uint32_t duration) {
    Bytes payload;
    putU32(payload, 0);
    putU32(payload, 0);
    putU32(payload, timescale);
    putU32(payload, duration);
    putU16(payload, 0x55C4);  // language
    putU16(payload, 0);
    return fullBox(kFourcc("mdhd"), 0, 0, payload);
}

Bytes hdlr(const char* handler) {
    Bytes payload;
    putU32(payload, 0);  // pre_defined
    putU32(payload, kFourcc(handler));
    for (int i = 0; i < 12; ++i) {
        payload.push_back(0);
    }
    payload.push_back(0);
    return fullBox(kFourcc("hdlr"), 0, 0, payload);
}

struct Mp4Spec {
    std::vector<uint32_t> sample_sizes;
    std::vector<size_t> keyframes;  // 0-based
    std::vector<int64_t> ctts;      // empty for no ctts box
    uint32_t timescale = 30000;
    uint32_t frame_delta = 1000;
    bool with_audio = false;
};

Bytes videoTrack(const Mp4Spec& spec, uint64_t first_sample_offset, uint32_t duration) {
    Bytes stbl_children;

    Bytes stsd_payload;
    putU32(stsd_payload, 1);  // entry count
    append(stsd_payload, visualSampleEntry());
    append(stbl_children, fullBox(kFourcc("stsd"), 0, 0, stsd_payload));

    Bytes stts_payload;
    putU32(stts_payload, 1);
    putU32(stts_payload, static_cast<uint32_t>(spec.sample_sizes.size()));
    putU32(stts_payload, spec.frame_delta);
    append(stbl_children, fullBox(kFourcc("stts"), 0, 0, stts_payload));

    if (!spec.ctts.empty()) {
        Bytes ctts_payload;
        putU32(ctts_payload, static_cast<uint32_t>(spec.ctts.size()));
        for (int64_t offset : spec.ctts) {
            putU32(ctts_payload, 1);
            putU32(ctts_payload, static_cast<uint32_t>(offset));
        }
        append(stbl_children, fullBox(kFourcc("ctts"), 0, 0, ctts_payload));
    }

    Bytes stss_payload;
    putU32(stss_payload, static_cast<uint32_t>(spec.keyframes.size()));
    for (size_t index : spec.keyframes) {
        putU32(stss_payload, static_cast<uint32_t>(index + 1));
    }
    append(stbl_children, fullBox(kFourcc("stss"), 0, 0, stss_payload));

    Bytes stsz_payload;
    putU32(stsz_payload, 0);
    putU32(stsz_payload, static_cast<uint32_t>(spec.sample_sizes.size()));
    for (uint32_t size : spec.sample_sizes) {
        putU32(stsz_payload, size);
    }
    append(stbl_children, fullBox(kFourcc("stsz"), 0, 0, stsz_payload));

    Bytes stsc_payload;
    putU32(stsc_payload, 1);
    putU32(stsc_payload, 1);
    putU32(stsc_payload, static_cast<uint32_t>(spec.sample_sizes.size()));
    putU32(stsc_payload, 1);
    append(stbl_children, fullBox(kFourcc("stsc"), 0, 0, stsc_payload));

    Bytes stco_payload;
    putU32(stco_payload, 1);
    putU32(stco_payload, static_cast<uint32_t>(first_sample_offset));
    append(stbl_children, fullBox(kFourcc("stco"), 0, 0, stco_payload));

    Bytes minf = box(kFourcc("minf"), box(kFourcc("stbl"), stbl_children));

    Bytes mdia_children;
    append(mdia_children, mdhd(spec.timescale, duration));
    append(mdia_children, hdlr("vide"));
    append(mdia_children, minf);

    Bytes trak_children;
    append(trak_children, tkhd(1, duration, 1280, 720));
    append(trak_children, box(kFourcc("mdia"), mdia_children));
    return box(kFourcc("trak"), trak_children);
}

Bytes audioTrack() {
    Bytes stbl_children;
    Bytes stsd_payload;
    putU32(stsd_payload, 1);
    append(stsd_payload, audioSampleEntry());
    append(stbl_children, fullBox(kFourcc("stsd"), 0, 0, stsd_payload));

    Bytes stts_payload;
    putU32(stts_payload, 1);
    putU32(stts_payload, 1);
    putU32(stts_payload, 1024);
    append(stbl_children, fullBox(kFourcc("stts"), 0, 0, stts_payload));

    Bytes stsz_payload;
    putU32(stsz_payload, 4);
    putU32(stsz_payload, 1);
    append(stbl_children, fullBox(kFourcc("stsz"), 0, 0, stsz_payload));

    Bytes stsc_payload;
    putU32(stsc_payload, 1);
    putU32(stsc_payload, 1);
    putU32(stsc_payload, 1);
    putU32(stsc_payload, 1);
    append(stbl_children, fullBox(kFourcc("stsc"), 0, 0, stsc_payload));

    Bytes stco_payload;
    putU32(stco_payload, 1);
    putU32(stco_payload, 0);
    append(stbl_children, fullBox(kFourcc("stco"), 0, 0, stco_payload));

    Bytes minf = box(kFourcc("minf"), box(kFourcc("stbl"), stbl_children));
    Bytes mdia_children;
    append(mdia_children, mdhd(44100, 5880));
    append(mdia_children, hdlr("soun"));
    append(mdia_children, minf);
    Bytes trak_children;
    append(trak_children, tkhd(2, 5880, 0, 0));
    append(trak_children, box(kFourcc("mdia"), mdia_children));
    return box(kFourcc("trak"), trak_children);
}

Bytes mvhd(uint32_t timescale, uint32_t duration) {
    Bytes payload;
    putU32(payload, 0);  // creation
    putU32(payload, 0);  // modification
    putU32(payload, timescale);
    putU32(payload, duration);
    putU32(payload, 0x00010000);  // rate
    putU16(payload, 0x0100);      // volume
    putU16(payload, 0);           // reserved
    for (int i = 0; i < 8; ++i) {
        payload.push_back(0);
    }
    const uint32_t matrix[9] = {0x00010000, 0, 0, 0, 0x00010000, 0, 0, 0, 0x40000000};
    for (uint32_t value : matrix) {
        putU32(payload, value);
    }
    for (int i = 0; i < 6; ++i) {
        putU32(payload, 0);
    }
    putU32(payload, 3);  // next_track_id
    return fullBox(kFourcc("mvhd"), 0, 0, payload);
}

Bytes buildMp4(const Mp4Spec& spec) {
    const uint32_t duration =
        static_cast<uint32_t>(spec.sample_sizes.size()) * spec.frame_delta;

    Bytes sample_data;
    for (size_t i = 0; i < spec.sample_sizes.size(); ++i) {
        const uint32_t size = spec.sample_sizes[i];
        Bytes sample(size, 0);
        const uint32_t nal_size = size >= 4 ? size - 4 : 0;
        sample[0] = static_cast<uint8_t>(nal_size >> 24);
        sample[1] = static_cast<uint8_t>((nal_size >> 16) & 0xFF);
        sample[2] = static_cast<uint8_t>((nal_size >> 8) & 0xFF);
        sample[3] = static_cast<uint8_t>(nal_size & 0xFF);
        sample[4] = 0x02;  // NAL header
        if (size > 5) {
            sample[5] = 0x01;
        }
        for (size_t k = 6; k < size; ++k) {
            sample[k] = static_cast<uint8_t>(0xAA + i);
        }
        append(sample_data, sample);
    }

    Bytes ftyp_payload;
    putU32(ftyp_payload, kFourcc("isom"));
    putU32(ftyp_payload, 0);
    putU32(ftyp_payload, kFourcc("isom"));
    putU32(ftyp_payload, kFourcc("iso2"));
    const Bytes ftyp = box(kFourcc("ftyp"), ftyp_payload);

    const Bytes mdat = box(kFourcc("mdat"), sample_data);
    const uint64_t first_sample_offset = ftyp.size() + 8;

    Bytes moov_children;
    append(moov_children, mvhd(1000, duration / 30 + 1));
    append(moov_children, videoTrack(spec, first_sample_offset, duration));
    if (spec.with_audio) {
        append(moov_children, audioTrack());
    }
    const Bytes moov = box(kFourcc("moov"), moov_children);

    Bytes file;
    append(file, ftyp);
    append(file, mdat);
    append(file, moov);
    return file;
}

std::filesystem::path writeMp4(const Bytes& data, const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path;
}

Mp4Spec simpleSpec() {
    Mp4Spec spec;
    spec.sample_sizes = {7, 6, 6, 6};
    spec.keyframes = {0, 2};
    spec.with_audio = true;
    return spec;
}

}  // namespace

TEST(Mp4Demuxer, ParsesVideoAndAudioTrack) {
    const std::filesystem::path path = writeMp4(buildMp4(simpleSpec()), "hevc_player_mp4_basic.mp4");
    {
        player::demux::Mp4Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        EXPECT_EQ(demuxer.video().codec, player::demux::Codec::Hevc);
        EXPECT_EQ(demuxer.video().width, 1280u);
        EXPECT_EQ(demuxer.video().height, 720u);
        EXPECT_EQ(demuxer.video().timescale, 30000u);
        EXPECT_FALSE(demuxer.video().format.annexb);
        EXPECT_EQ(demuxer.video().format.nal_length_size, 4);
        EXPECT_FALSE(demuxer.video().hvcc.empty());
        EXPECT_DOUBLE_EQ(demuxer.video().fps, 30.0);
        EXPECT_NEAR(demuxer.duration(), 4000.0 / 30000.0, 1e-9);

        EXPECT_TRUE(demuxer.audio().present);
        EXPECT_EQ(demuxer.audio().codec_fourcc, kFourcc("mp4a"));
        EXPECT_EQ(demuxer.audio().channels, 2u);
        EXPECT_EQ(demuxer.audio().sample_rate, 44100u);
    }
    std::filesystem::remove(path);
}

TEST(Mp4Demuxer, ReadsSamplesWithPtsAndKeyframes) {
    const std::filesystem::path path = writeMp4(buildMp4(simpleSpec()), "hevc_player_mp4_samples.mp4");
    {
        player::demux::Mp4Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 0);
        EXPECT_TRUE(sample.keyframe);
        EXPECT_EQ(sample.data.size(), 7u);
        EXPECT_EQ(sample.data[3], 3u);
        EXPECT_EQ(sample.data[4], 0x02u);

        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 1000);
        EXPECT_FALSE(sample.keyframe);
        EXPECT_EQ(sample.data.size(), 6u);

        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 2000);
        EXPECT_TRUE(sample.keyframe);

        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 3000);
        EXPECT_FALSE(sample.keyframe);

        EXPECT_FALSE(demuxer.readSample(sample));
    }
    std::filesystem::remove(path);
}

TEST(Mp4Demuxer, SeeksToPreviousKeyframe) {
    const std::filesystem::path path = writeMp4(buildMp4(simpleSpec()), "hevc_player_mp4_seek.mp4");
    {
        player::demux::Mp4Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        ASSERT_TRUE(demuxer.seek(0.08));  // 2400 ticks -> previous keyframe at 2000
        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 2000);
        EXPECT_TRUE(sample.keyframe);

        ASSERT_TRUE(demuxer.seek(0.0));
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 0);
    }
    std::filesystem::remove(path);
}

TEST(Mp4Demuxer, AppliesCompositionOffsets) {
    Mp4Spec spec;
    spec.sample_sizes = {6, 6, 6};
    spec.keyframes = {0};
    spec.ctts = {2000, 0, 1000};

    const std::filesystem::path path = writeMp4(buildMp4(spec), "hevc_player_mp4_ctts.mp4");
    {
        player::demux::Mp4Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.dts, 0);
        EXPECT_EQ(sample.pts, 2000);
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 1000);
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 3000);
    }
    std::filesystem::remove(path);
}

TEST(Mp4Demuxer, FactoryDetectsMp4) {
    const std::filesystem::path path = writeMp4(buildMp4(simpleSpec()), "hevc_player_mp4_factory.mp4");
    {
        std::string error;
        std::unique_ptr<player::demux::IDemuxer> demuxer = player::demux::createDemuxer(path, error);
        ASSERT_TRUE(demuxer) << error;
        EXPECT_EQ(demuxer->video().codec, player::demux::Codec::Hevc);
        EXPECT_EQ(demuxer->video().width, 1280u);
    }
    std::filesystem::remove(path);
}
