#include "audio/ac3/Ac3Decoder.hpp"
#include "demux/Ac3Demuxer.hpp"
#include "demux/Demuxer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
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

// One AC-3 syncframe with a valid header and zero payload. 48 kHz, 160 kbps
// (frame size code 18 -> 640 bytes).
Bytes ac3Frame(uint8_t acmod, bool lfe_on) {
    constexpr uint32_t kSize = 640;
    Bytes frame(kSize, 0);
    frame[0] = 0x0B;
    frame[1] = 0x77;
    frame[4] = 18;                            // fscod 0, frmsizecod 18
    frame[5] = static_cast<uint8_t>(8 << 3);  // bsid 8, bsmod 0
    int lfe_bit = 4;
    if (acmod == 2) {
        lfe_bit = 2;  // dsurmod
    } else {
        if ((acmod & 1) && acmod != 1) {
            lfe_bit -= 2;  // cmixlev
        }
        if (acmod & 4) {
            lfe_bit -= 2;  // surmixlev
        }
    }
    frame[6] = static_cast<uint8_t>((acmod << 5) | (lfe_on ? (1u << lfe_bit) : 0u));
    return frame;
}

Bytes buildRawAc3(int frames, uint8_t acmod = 2, bool lfe_on = false) {
    Bytes file;
    for (int i = 0; i < frames; ++i) {
        const Bytes frame = ac3Frame(acmod, lfe_on);
        file.insert(file.end(), frame.begin(), frame.end());
    }
    return file;
}

std::filesystem::path writeFile(const Bytes& data, const std::string& name) {
    const std::filesystem::path path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    return path;
}

}  // namespace

TEST(Ac3Demuxer, FrameSizeParsing) {
    Bytes frame = ac3Frame(2, false);
    EXPECT_EQ(player::demux::Ac3Demuxer::frameSize(frame.data(), frame.size()), 640u);
    frame[0] = 0;
    EXPECT_EQ(player::demux::Ac3Demuxer::frameSize(frame.data(), frame.size()), 0u);
    frame = ac3Frame(2, false);
    frame[5] = static_cast<uint8_t>(16 << 3);  // E-AC-3 bitstream id
    EXPECT_EQ(player::demux::Ac3Demuxer::frameSize(frame.data(), frame.size()), 0u);
}

TEST(Ac3Demuxer, ParsesFramesAndSeeks) {
    const Bytes file = buildRawAc3(3);
    const std::filesystem::path path = writeFile(file, "fvideoplayer_ac3_basic.ac3");
    {
        player::demux::Ac3Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;
        ASSERT_TRUE(demuxer.audio().present);
        EXPECT_EQ(demuxer.audio().codec, "ac-3");
        EXPECT_EQ(demuxer.audio().codec_fourcc, 0x61632D33u);
        EXPECT_EQ(demuxer.audio().sample_rate, 48000u);
        EXPECT_EQ(demuxer.audio().channels, 2u);
        EXPECT_DOUBLE_EQ(demuxer.duration(), 3.0 * 1536.0 / 48000.0);
        EXPECT_EQ(demuxer.video().codec, player::demux::Codec::Unknown);

        player::demux::Sample sample;
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 0);
        EXPECT_EQ(sample.data.size(), 640u);
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 32);
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 64);
        EXPECT_FALSE(demuxer.readAudioSample(sample));

        // A seek lands on the frame that covers the target time.
        ASSERT_TRUE(demuxer.seek(0.05));
        ASSERT_TRUE(demuxer.readAudioSample(sample));
        EXPECT_EQ(sample.pts, 32);
    }
    std::filesystem::remove(path);
}

TEST(Ac3Demuxer, ReadsSixChannelTrack) {
    const Bytes file = buildRawAc3(1, 7, true);
    const std::filesystem::path path = writeFile(file, "fvideoplayer_ac3_51.ac3");
    {
        player::demux::Ac3Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(path, error)) << error;
        EXPECT_EQ(demuxer.audio().channels, 6u);
    }
    std::filesystem::remove(path);
}

TEST(Ac3Demuxer, DetectsRawAc3Files) {
    const Bytes file = buildRawAc3(2);
    const std::filesystem::path path = writeFile(file, "fvideoplayer_ac3_detect.ac3");
    {
        std::string error;
        const std::unique_ptr<player::demux::IDemuxer> demuxer =
            player::demux::createDemuxer(path, error);
        ASSERT_TRUE(demuxer != nullptr) << error;
        ASSERT_TRUE(demuxer->audio().present);
        EXPECT_EQ(demuxer->audio().codec, "ac-3");
        EXPECT_EQ(demuxer->audio().channels, 2u);
    }
    std::filesystem::remove(path);
}

TEST(Ac3Demuxer, ReadsMonoTrack) {
    const Bytes plain = buildRawAc3(1, 1, false);
    const std::filesystem::path plain_path = writeFile(plain, "fvideoplayer_ac3_mono.ac3");
    {
        player::demux::Ac3Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(plain_path, error)) << error;
        EXPECT_EQ(demuxer.audio().channels, 1u);
    }
    std::filesystem::remove(plain_path);

    const Bytes with_lfe = buildRawAc3(1, 1, true);
    const std::filesystem::path lfe_path = writeFile(with_lfe, "fvideoplayer_ac3_mono_lfe.ac3");
    {
        player::demux::Ac3Demuxer demuxer;
        std::string error;
        ASSERT_TRUE(demuxer.open(lfe_path, error)) << error;
        EXPECT_EQ(demuxer.audio().channels, 2u);
    }
    std::filesystem::remove(lfe_path);
}

TEST(Ac3Decoder, DecodesAnExternalStreamWhenProvided) {
    const char* src = std::getenv("AC3_DECODE_SRC");
    const char* dst = std::getenv("AC3_DECODE_OUT");
    if (src == nullptr || dst == nullptr) {
        GTEST_SKIP();
    }
    std::ifstream in(src, std::ios::binary);
    ASSERT_TRUE(in.good());
    const Bytes data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    player::audio::ac3::Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.configure(nullptr, 0, error)) << error;

    std::ofstream out(dst, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good());

    size_t offset = 0;
    int frames = 0;
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    while (offset + 8 <= data.size()) {
        const uint32_t size = player::demux::Ac3Demuxer::frameSize(data.data() + offset,
                                                                   data.size() - offset);
        if (size == 0 || offset + size > data.size()) {
            break;
        }
        player::audio::PcmBuffer pcm;
        const player::audio::ac3::Status status =
            decoder.decode(data.data() + offset, size, pcm, error);
        ASSERT_EQ(status, player::audio::ac3::Status::Ok) << error << " at frame " << frames;
        channels = pcm.channels;
        sample_rate = pcm.sample_rate;
        ASSERT_FALSE(pcm.samples.empty());
        out.write(reinterpret_cast<const char*>(pcm.samples.data()),
                  static_cast<std::streamsize>(pcm.samples.size() * sizeof(float)));
        offset += size;
        ++frames;
    }
    EXPECT_GT(frames, 0);
    EXPECT_GT(channels, 0u);
    EXPECT_EQ(sample_rate, 48000u);
}
