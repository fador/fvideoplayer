#include "demux/Demuxer.hpp"
#include "demux/FlvDemuxer.hpp"
#include "playback/PlaybackEngine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Bytes = std::vector<uint8_t>;

void putU16(Bytes& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void putU24(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void putU32(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void putDouble(Bytes& out, double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<uint8_t>((bits >> (i * 8)) & 0xFF));
    }
}

void append(Bytes& out, const Bytes& data) {
    out.insert(out.end(), data.begin(), data.end());
}

void putString(Bytes& out, const std::string& text) {
    putU16(out, static_cast<uint16_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

// One FLV tag: header, payload and the trailing previous-tag size.
Bytes flvTag(uint8_t type, int64_t timestamp, const Bytes& payload) {
    Bytes out;
    out.push_back(type);
    putU24(out, static_cast<uint32_t>(payload.size()));
    putU24(out, static_cast<uint32_t>(timestamp & 0xFFFFFF));
    out.push_back(static_cast<uint8_t>((timestamp >> 24) & 0xFF));
    putU24(out, 0);  // stream id
    append(out, payload);
    putU32(out, static_cast<uint32_t>(payload.size()) + 11);
    return out;
}

// A script tag carrying the fields the demuxer reads from onMetaData.
Bytes metadataTag(double duration, double width, double height, double framerate) {
    Bytes payload;
    payload.push_back(0x02);  // string
    putString(payload, "onMetaData");
    payload.push_back(0x08);  // ECMA array
    putU32(payload, 4);
    const struct {
        const char* key;
        double value;
    } fields[] = {
        {"duration", duration}, {"width", width}, {"height", height}, {"framerate", framerate},
    };
    for (const auto& field : fields) {
        putString(payload, field.key);
        payload.push_back(0x00);  // number
        putDouble(payload, field.value);
    }
    putU16(payload, 0);       // empty key
    payload.push_back(0x09);  // object end
    return flvTag(18, 0, payload);
}

// A minimal but valid FLV1 keyframe: a 320x240 picture with one flat intra
// macroblock and the rest skipped, enough for the demuxer's size probe.
Bytes flv1VideoPayload(uint8_t frame_type, int64_t timestamp) {
    struct Writer {
        Bytes data;
        size_t count = 0;
        void bit(uint32_t value) {
            if ((count & 7) == 0) {
                data.push_back(0);
            }
            data.back() = static_cast<uint8_t>(data.back() | ((value & 1) << (7 - (count & 7))));
            ++count;
        }
        void bits(uint32_t value, unsigned n) {
            for (unsigned i = 0; i < n; ++i) {
                bit((value >> (n - 1 - i)) & 1u);
            }
        }
    };
    Bytes payload;
    payload.push_back(static_cast<uint8_t>((frame_type << 4) | 2));
    Writer writer;
    writer.bits(1, 17);
    writer.bits(1, 5);  // FLV escape format
    writer.bits(static_cast<uint32_t>(timestamp & 0xFF), 8);
    writer.bits(5, 3);  // 320x240
    writer.bits(frame_type == 1 ? 0 : 1, 2);
    writer.bits(0, 1);
    writer.bits(8, 5);
    writer.bits(0, 1);
    if (frame_type == 1) {
        writer.bits(1, 1);  // intra MCBPC: cbpc = 0
        writer.bits(3, 4);  // CBPY: 0
        for (int i = 0; i < 6; ++i) {
            writer.bits(128, 8);  // flat DC
        }
    } else {
        writer.bits(1, 1);  // skip macroblock
    }
    append(payload, writer.data);
    return payload;
}

Bytes mp3AudioTag() {
    Bytes payload;
    payload.push_back(0x2F);  // MP3, 44.1 kHz, stereo
    payload.push_back(0xFF);
    payload.push_back(0xFB);
    payload.push_back(0x90);
    payload.push_back(0x00);
    return flvTag(8, 0, payload);
}

// One full 128 kbps 44.1 kHz MP3 frame, silent. The frame size follows from
// the bitrate: 144 * 128000 / 44100 = 417 bytes.
Bytes mp3FrameTag(int64_t timestamp) {
    Bytes payload;
    payload.push_back(0x2F);  // MP3, 44.1 kHz, stereo
    payload.push_back(0xFF);
    payload.push_back(0xFB);
    payload.push_back(0x90);
    payload.push_back(0x00);
    payload.resize(418, 0);
    return flvTag(8, timestamp, payload);
}

Bytes aacConfigTag() {
    Bytes payload;
    payload.push_back(0xAF);  // AAC, 44.1 kHz, stereo
    payload.push_back(0x00);  // sequence header
    payload.push_back(0x12);  // AudioSpecificConfig: 44100 Hz stereo LC
    payload.push_back(0x10);
    return flvTag(8, 0, payload);
}

Bytes aacAudioTag() {
    Bytes payload;
    payload.push_back(0xAF);
    payload.push_back(0x01);  // raw AAC
    payload.push_back(0xDE);
    payload.push_back(0xAD);
    return flvTag(8, 20, payload);
}

Bytes buildFlv(const std::vector<Bytes>& tags, bool with_metadata = true, bool with_audio = true) {
    Bytes file = {'F', 'L', 'V', 1, static_cast<uint8_t>(0x01 | (with_audio ? 0x04 : 0x00)),
                  0, 0, 0, 9};
    putU32(file, 0);  // previous tag size 0
    if (with_metadata) {
        append(file, metadataTag(4.0, 320, 240, 25.0));
    }
    for (const Bytes& tag : tags) {
        append(file, tag);
    }
    return file;
}

std::filesystem::path writeFlv(const Bytes& data, const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path;
}

}  // namespace

TEST(FlvDemuxer, ParsesTracksAndMetadata) {
    const Bytes file = buildFlv({
        flvTag(9, 0, flv1VideoPayload(1, 0)),
        mp3AudioTag(),
        flvTag(9, 40, flv1VideoPayload(2, 40)),
    });
    const std::filesystem::path path = writeFlv(file, "hevc_player_flv_basic.flv");
    {
        player::demux::FlvDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        EXPECT_EQ(demuxer.video().codec, player::demux::Codec::Flv1);
        EXPECT_EQ(demuxer.video().width, 320u);
        EXPECT_EQ(demuxer.video().height, 240u);
        EXPECT_DOUBLE_EQ(demuxer.video().fps, 25.0);
        EXPECT_EQ(demuxer.video().timescale, 1000u);
        EXPECT_DOUBLE_EQ(demuxer.duration(), 4.0);

        ASSERT_TRUE(demuxer.audio().present);
        EXPECT_EQ(demuxer.audio().codec, "mp3");
        EXPECT_EQ(demuxer.audio().codec_fourcc, 0x6D703320u);
        EXPECT_EQ(demuxer.audio().sample_rate, 44100u);
        EXPECT_EQ(demuxer.audio().channels, 2u);

        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 0);
        EXPECT_TRUE(sample.keyframe);
        // The FLV frame-type/codec byte is stripped, leaving the H.263 start
        // code at the front.
        ASSERT_GE(sample.data.size(), 2u);
        EXPECT_EQ(sample.data[0], 0u);
        EXPECT_EQ(sample.data[1], 0u);
        EXPECT_EQ(sample.track, player::demux::TrackType::Video);

        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 40);
        EXPECT_FALSE(sample.keyframe);
        EXPECT_FALSE(demuxer.readSample(sample));

        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 0);
        EXPECT_EQ(sample.data.size(), 4u);
        EXPECT_EQ(sample.data[0], 0xFFu);
        EXPECT_FALSE(demuxer.readAudioSample(sample));
    }
    std::filesystem::remove(path);
}

TEST(FlvDemuxer, DetectsDimensionsWithoutMetadata) {
    const Bytes file = buildFlv({flvTag(9, 0, flv1VideoPayload(1, 0))}, false, false);
    const std::filesystem::path path = writeFlv(file, "hevc_player_flv_nometa.flv");
    {
        player::demux::FlvDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;
        EXPECT_EQ(demuxer.video().width, 320u);
        EXPECT_EQ(demuxer.video().height, 240u);
    }
    std::filesystem::remove(path);
}

TEST(FlvDemuxer, SeeksToPreviousKeyframe) {
    const Bytes file = buildFlv({
        flvTag(9, 0, flv1VideoPayload(1, 0)),
        flvTag(9, 40, flv1VideoPayload(2, 40)),
        flvTag(9, 80, flv1VideoPayload(1, 80)),
        flvTag(9, 120, flv1VideoPayload(2, 120)),
    });
    const std::filesystem::path path = writeFlv(file, "hevc_player_flv_seek.flv");
    {
        player::demux::FlvDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;
        ASSERT_TRUE(demuxer.seek(0.1));
        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.pts, 80);
        EXPECT_TRUE(sample.keyframe);
    }
    std::filesystem::remove(path);
}

TEST(FlvDemuxer, ReadsAacSequenceHeaderAsExtradata) {
    const Bytes file = buildFlv({
        flvTag(9, 0, flv1VideoPayload(1, 0)),
        aacConfigTag(),
        aacAudioTag(),
    });
    const std::filesystem::path path = writeFlv(file, "hevc_player_flv_aac.flv");
    {
        player::demux::FlvDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;
        EXPECT_EQ(demuxer.audio().codec, "aac");
        EXPECT_EQ(demuxer.audio().codec_fourcc, 0x6D703461u);
        EXPECT_EQ(demuxer.audio().sample_rate, 44100u);
        EXPECT_EQ(demuxer.audio().channels, 2u);
        ASSERT_EQ(demuxer.audio().extradata.size(), 2u);
        EXPECT_EQ(demuxer.audio().extradata[0], 0x12u);

        // The sequence header is configuration, not a sample.
        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 20);
        EXPECT_EQ(sample.data.size(), 2u);
        EXPECT_EQ(sample.data[0], 0xDEu);
    }
    std::filesystem::remove(path);
}

TEST(FlvDemuxer, FactoryDetectsFlv) {
    const Bytes file = buildFlv({flvTag(9, 0, flv1VideoPayload(1, 0))});
    const std::filesystem::path path = writeFlv(file, "hevc_player_flv_factory.flv");
    {
        std::string error;
        std::unique_ptr<player::demux::IDemuxer> demuxer =
            player::demux::createDemuxer(path, error);
        ASSERT_NE(demuxer, nullptr) << error;
        EXPECT_EQ(demuxer->video().codec, player::demux::Codec::Flv1);
    }
    std::filesystem::remove(path);
}

TEST(FlvDemuxer, RejectsUnsupportedVideoWithoutAudio) {
    Bytes payload;
    payload.push_back(0x17);  // keyframe, AVC
    payload.push_back(0x00);
    const Bytes file = buildFlv({flvTag(9, 0, payload)}, true, false);
    const std::filesystem::path path = writeFlv(file, "hevc_player_flv_avc.flv");
    {
        player::demux::FlvDemuxer demuxer;
        std::string error;
        EXPECT_FALSE(demuxer.open(path, error));
        EXPECT_NE(error.find("H.264"), std::string::npos) << error;
    }
    std::filesystem::remove(path);
}

TEST(FlvDemuxer, KeepsUnsupportedVideoAndPlaysAudio) {
    Bytes keyframe;
    keyframe.push_back(0x17);  // keyframe, AVC
    keyframe.push_back(0x00);
    Bytes inter;
    inter.push_back(0x27);  // inter frame, AVC
    inter.push_back(0x01);
    const Bytes file = buildFlv({
        flvTag(9, 0, keyframe),
        mp3AudioTag(),
        flvTag(9, 40, inter),
    });
    const std::filesystem::path path = writeFlv(file, "hevc_player_flv_avc_audio.flv");
    {
        player::demux::FlvDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;
        EXPECT_EQ(demuxer.video().codec, player::demux::Codec::Unknown);
        EXPECT_EQ(demuxer.video().codec_name, "H.264");
        EXPECT_EQ(demuxer.video().width, 320u);
        EXPECT_EQ(demuxer.video().height, 240u);
        ASSERT_TRUE(demuxer.audio().present);
        EXPECT_EQ(demuxer.audio().codec, "mp3");

        // The video track has no decoder, so it yields no samples; the audio
        // cursor is independent and keeps working, seek included.
        player::demux::Sample sample;
        EXPECT_FALSE(demuxer.readSample(sample));
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.track, player::demux::TrackType::Audio);
        EXPECT_TRUE(demuxer.seek(0.0));
        ASSERT_TRUE(demuxer.readAudioSample(sample));
    }
    std::filesystem::remove(path);
}

TEST(FlvDemuxer, EnginePlaysAudioOfUnsupportedVideo) {
    Bytes keyframe;
    keyframe.push_back(0x17);  // keyframe, AVC
    keyframe.push_back(0x00);
    std::vector<Bytes> tags;
    tags.push_back(flvTag(9, 0, keyframe));
    for (int i = 0; i < 8; ++i) {
        tags.push_back(mp3FrameTag(i * 26));
    }
    const Bytes file = buildFlv(tags);
    const std::filesystem::path path = writeFlv(file, "hevc_player_flv_avc_engine.flv");
    {
        player::playback::PlaybackEngine engine;
        std::string error;
        ASSERT_TRUE(engine.open(path, error)) << error;
        EXPECT_EQ(engine.videoInfo().codec, player::demux::Codec::Unknown);
        EXPECT_EQ(engine.videoInfo().codec_name, "H.264");
        EXPECT_EQ(std::string(engine.audioCodecName()), "MP3");
        EXPECT_TRUE(engine.audioDecoding());
        engine.play();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (engine.audioFrames() == 0 && std::chrono::steady_clock::now() < deadline) {
            engine.update();
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_GT(engine.audioFrames(), 0u);
        engine.close();
    }
    std::filesystem::remove(path);
}
