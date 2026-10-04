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

// Big-endian helper used by the hvcC record.
void putU16(Bytes& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void putU16Le(Bytes& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>(value >> 8));
}

// Big-endian helper used for ASCII fourcc tags.
void putU32(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

void putU32Le(Bytes& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

void append(Bytes& out, const Bytes& data) {
    out.insert(out.end(), data.begin(), data.end());
}

Bytes chunk(uint32_t id, const Bytes& payload) {
    Bytes out;
    putU32(out, id);
    putU32Le(out, static_cast<uint32_t>(payload.size()));
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
    putU32Le(payload, 33333);  // dwMicroSecPerFrame
    putU32Le(payload, 0);      // dwMaxBytesPerSec
    putU32Le(payload, 0);      // dwPaddingGranularity
    putU32Le(payload, 0x10);   // dwFlags
    putU32Le(payload, 4);      // dwTotalFrames
    putU32Le(payload, 0);      // dwInitialFrames
    putU32Le(payload, 1);      // dwStreams
    putU32Le(payload, 0);      // dwSuggestedBufferSize
    putU32Le(payload, 1280);   // dwWidth
    putU32Le(payload, 720);    // dwHeight
    for (int i = 0; i < 4; ++i) {
        putU32Le(payload, 0);
    }
    return chunk(kFourcc("avih"), payload);
}

Bytes strh(uint32_t fourcc = kFourcc("HEVC")) {
    Bytes payload;
    putU32(payload, kFourcc("vids"));
    putU32(payload, fourcc);
    putU32Le(payload, 0);  // dwFlags
    putU16Le(payload, 0);  // wPriority
    putU16Le(payload, 0);  // wLanguage
    putU32Le(payload, 0);  // dwInitialFrames
    putU32Le(payload, 1);  // dwScale
    putU32Le(payload, 30);  // dwRate
    putU32Le(payload, 0);  // dwStart
    putU32Le(payload, 4);  // dwLength
    putU32Le(payload, 0);  // dwSuggestedBufferSize
    putU32Le(payload, 0);  // dwQuality
    putU32Le(payload, 0);  // dwSampleSize
    putU32Le(payload, 0);  // rcFrame left/top
    putU32Le(payload, 0);  // rcFrame right/bottom
    return chunk(kFourcc("strh"), payload);
}

Bytes strfVideo(bool annexb, uint32_t fourcc = kFourcc("HEVC")) {
    Bytes payload;
    putU32Le(payload, 40);  // biSize
    putU32Le(payload, 1280);
    putU32Le(payload, 720);
    putU16Le(payload, 1);   // biPlanes
    putU16Le(payload, 24);  // biBitCount
    putU32(payload, fourcc);
    putU32Le(payload, 0);
    putU32Le(payload, 0);
    putU32Le(payload, 0);
    putU32Le(payload, 0);
    putU32Le(payload, 0);
    if (fourcc == kFourcc("HEVC")) {
        if (annexb) {
            const Bytes extradata = {0, 0, 0, 1, 0x40, 0x01, 0x0C};
            append(payload, extradata);
        } else {
            append(payload, makeHvcc());
        }
    }
    return chunk(kFourcc("strf"), payload);
}

// Audio stream header. AVI stores no per packet timing, so scale and rate
// describe one chunk, here 1152 samples of a 44.1 kHz MP3 frame.
Bytes strhAudio(uint32_t scale, uint32_t rate, uint32_t length) {
    Bytes payload;
    putU32(payload, kFourcc("auds"));
    putU32(payload, 0);
    putU32Le(payload, 0);  // dwFlags
    putU16Le(payload, 0);  // wPriority
    putU16Le(payload, 0);  // wLanguage
    putU32Le(payload, 0);  // dwInitialFrames
    putU32Le(payload, scale);
    putU32Le(payload, rate);
    putU32Le(payload, 0);  // dwStart
    putU32Le(payload, length);
    putU32Le(payload, 0);  // dwSuggestedBufferSize
    putU32Le(payload, 0);  // dwQuality
    putU32Le(payload, 0);  // dwSampleSize
    putU32Le(payload, 0);  // rcFrame left
    putU32Le(payload, 0);  // rcFrame top
    putU32Le(payload, 0);  // rcFrame right
    putU32Le(payload, 0);  // rcFrame bottom
    return chunk(kFourcc("strh"), payload);
}

// WAVEFORMATEX with the trailing codec specific bytes real writers append.
Bytes strfAudio(uint16_t format_tag, uint16_t channels, uint32_t sample_rate,
                uint16_t block_align, uint16_t bits, const Bytes& extra) {
    Bytes payload;
    putU16Le(payload, format_tag);
    putU16Le(payload, channels);
    putU32Le(payload, sample_rate);
    putU32Le(payload, sample_rate);  // nAvgBytesPerSec, approximate
    putU16Le(payload, block_align);
    putU16Le(payload, bits);
    putU16Le(payload, static_cast<uint16_t>(extra.size()));
    append(payload, extra);
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

struct AviSpec {
    bool with_idx1 = true;
    bool annexb = false;
    bool with_audio = false;
    bool aac = false;  // MP3 by default, AAC with the AudioSpecificConfig extra
    uint32_t video_fourcc = 0;  // 0 = HEVC
};

AviSpec audioSpec() {
    AviSpec spec;
    spec.with_audio = true;
    return spec;
}

AviSpec aacSpec() {
    AviSpec spec;
    spec.with_audio = true;
    spec.aac = true;
    return spec;
}

AviSpec audioScanSpec() {
    AviSpec spec;
    spec.with_audio = true;
    spec.with_idx1 = false;
    return spec;
}

AviSpec noIdx1() {
    AviSpec spec;
    spec.with_idx1 = false;
    return spec;
}

AviSpec annexbSpec() {
    AviSpec spec;
    spec.annexb = true;
    return spec;
}

Bytes buildAvi(AviSpec spec) {
    const bool with_idx1 = spec.with_idx1;
    const bool annexb = spec.annexb;
    const uint32_t video_fourcc = spec.video_fourcc != 0 ? spec.video_fourcc : kFourcc("HEVC");

    Bytes hdrl_payload = avih();
    Bytes strl_payload = strh(video_fourcc);
    append(strl_payload, strfVideo(annexb, video_fourcc));
    append(hdrl_payload, listChunk("strl", strl_payload));

    Bytes audio_strl;
    if (spec.with_audio) {
        const Bytes extra = spec.aac ? Bytes{0x12, 0x08} : Bytes{0x00, 0x00};
        append(audio_strl, strhAudio(1152, 44100, 3));
        append(audio_strl, strfAudio(spec.aac ? 0x00FF : 0x0055, 1, 44100, 1152, 16, extra));
        append(hdrl_payload, listChunk("strl", audio_strl));
    }
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
    std::vector<uint32_t> audio_offsets;
    std::vector<uint32_t> audio_sizes;
    if (spec.with_audio) {
        for (size_t i = 0; i < 3; ++i) {
            const Bytes data = {0xFF, 0xFB, static_cast<uint8_t>(0x90 + i),
                                static_cast<uint8_t>(0xC4 + i)};
            audio_offsets.push_back(static_cast<uint32_t>(movi_data.size()));
            audio_sizes.push_back(static_cast<uint32_t>(data.size()));
            append(movi_data, chunk(kFourcc("01wb"), data));
        }
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
            putU32Le(entries, keyframes[i] ? 0x10 : 0);
            putU32Le(entries, offsets[i]);
            putU32Le(entries, sizes[i]);
        }
        for (size_t i = 0; i < audio_offsets.size(); ++i) {
            putU32(entries, kFourcc("01wb"));
            putU32Le(entries, 0);
            putU32Le(entries, audio_offsets[i]);
            putU32Le(entries, audio_sizes[i]);
        }
        append(riff_body, chunk(kFourcc("idx1"), entries));
    }
    putU32Le(file, static_cast<uint32_t>(riff_body.size()));
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
    const std::filesystem::path path = writeAvi(buildAvi({}), "hevc_player_avi_basic.avi");
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

TEST(AviDemuxer, KeepsUnsupportedVideoAndPlaysAudio) {
    AviSpec spec = audioSpec();
    spec.video_fourcc = kFourcc("H264");
    const std::filesystem::path path = writeAvi(buildAvi(spec), "hevc_player_avi_h264.avi");
    {
        player::demux::AviDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        EXPECT_EQ(demuxer.video().codec, player::demux::Codec::Unknown);
        EXPECT_EQ(demuxer.video().codec_name, "H.264");
        EXPECT_EQ(demuxer.video().width, 1280u);
        EXPECT_EQ(demuxer.video().height, 720u);
        EXPECT_TRUE(demuxer.audio().present);
        // Packet times are whole milliseconds, so three 1152-sample chunks at
        // 44.1 kHz end at 78 ms.
        EXPECT_NEAR(demuxer.duration(), 0.078, 1e-6);

        player::demux::Sample sample;
        EXPECT_FALSE(demuxer.readSample(sample));
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.track, player::demux::TrackType::Audio);
        EXPECT_TRUE(demuxer.seek(0.05));
        ASSERT_TRUE(demuxer.readAudioSample(sample));
    }
    std::filesystem::remove(path);
}

TEST(AviDemuxer, ScansMoviWithoutIdx1) {
    const std::filesystem::path path = writeAvi(buildAvi(noIdx1()), "hevc_player_avi_scan.avi");
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
    const std::filesystem::path path = writeAvi(buildAvi(annexbSpec()), "hevc_player_avi_annexb.avi");
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
    const std::filesystem::path path = writeAvi(buildAvi({}), "hevc_player_avi_seek.avi");
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
    const std::filesystem::path path = writeAvi(buildAvi({}), "hevc_player_avi_factory.avi");
    {
        std::string error;
        std::unique_ptr<player::demux::IDemuxer> demuxer = player::demux::createDemuxer(path, error);
        ASSERT_TRUE(demuxer) << error;
        EXPECT_EQ(demuxer->video().codec, player::demux::Codec::Hevc);
    }
    std::filesystem::remove(path);
}

TEST(AviDemuxer, ReadsAudioTrackAndPackets) {
    const std::filesystem::path path = writeAvi(buildAvi(audioSpec()),
                                                "hevc_player_avi_audio.avi");
    {
        player::demux::AviDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        const std::vector<player::demux::AudioTrackInfo> tracks = demuxer.audioTracks();
        ASSERT_EQ(tracks.size(), 1u);
        EXPECT_EQ(tracks[0].codec_fourcc, kFourcc("mp3 "));
        EXPECT_EQ(tracks[0].channels, 1u);
        EXPECT_EQ(tracks[0].sample_rate, 44100u);
        EXPECT_TRUE(tracks[0].default_track);
        // MP3 needs no decoder configuration, the WAVEFORMATEX extras are
        // only meaningful for the codecs that carry their own header.
        EXPECT_TRUE(tracks[0].extradata.empty());

        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 0);
        EXPECT_EQ(sample.track, player::demux::TrackType::Audio);
        ASSERT_EQ(sample.data.size(), 4u);
        EXPECT_EQ(sample.data[0], 0xFFu);  // MPEG frame sync

        // 1152 samples at 44.1 kHz per chunk.
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 26);
        EXPECT_EQ(sample.data[2], 0x91u);
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 52);
        EXPECT_EQ(sample.data[2], 0x92u);
        EXPECT_FALSE(demuxer.readAudioSample(sample));
    }
    std::filesystem::remove(path);
}

TEST(AviDemuxer, ReadsAacExtradataFromStrf) {
    const std::filesystem::path path = writeAvi(buildAvi(aacSpec()),
                                                "hevc_player_avi_aac.avi");
    {
        player::demux::AviDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        const std::vector<player::demux::AudioTrackInfo> tracks = demuxer.audioTracks();
        ASSERT_EQ(tracks.size(), 1u);
        EXPECT_EQ(tracks[0].codec_fourcc, kFourcc("mp4a"));
        // AAC in AVI carries raw access units, so the AudioSpecificConfig from
        // the WAVEFORMATEX trailing bytes is the decoder configuration.
        ASSERT_EQ(tracks[0].extradata.size(), 2u);
        EXPECT_EQ(tracks[0].extradata[0], 0x12u);
        EXPECT_EQ(tracks[0].extradata[1], 0x08u);
    }
    std::filesystem::remove(path);
}

TEST(AviDemuxer, ScansAudioChunksWithoutIdx1) {
    const std::filesystem::path path =
        writeAvi(buildAvi(audioScanSpec()), "hevc_player_avi_audio_scan.avi");
    {
        player::demux::AviDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        player::demux::Sample sample;
        int packets = 0;
        while (demuxer.readAudioSample(sample)) {
            ++packets;
        }
        EXPECT_EQ(packets, 3);
        // Video stays readable: the scan must not consume the 00dc chunks.
        ASSERT_TRUE(demuxer.readSample(sample));
        EXPECT_EQ(sample.data[4], 0x26u);
    }
    std::filesystem::remove(path);
}

TEST(AviDemuxer, SeekRepositionsAudioCursor) {
    const std::filesystem::path path = writeAvi(buildAvi(audioSpec()),
                                                "hevc_player_avi_seek_audio.avi");
    {
        player::demux::AviDemuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;

        player::demux::Sample sample;
        while (demuxer.readAudioSample(sample)) {
        }
        ASSERT_FALSE(demuxer.readAudioSample(sample));

        // A backward seek has to rewind the audio cursor too, otherwise the
        // player runs dry and stays silent.
        ASSERT_TRUE(demuxer.seek(0.0));
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 0);
        while (demuxer.readAudioSample(sample)) {
        }

        // 30 ms falls inside the chunk that starts at 26 ms.
        ASSERT_TRUE(demuxer.seek(0.03));
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 26);
    }
    std::filesystem::remove(path);
}
