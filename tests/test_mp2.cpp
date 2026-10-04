#include "audio/mp2/Mp2Decoder.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

using player::audio::PcmBuffer;
using player::audio::mp2::Decoder;
using player::audio::mp2::Status;

// Frame length from the header, for walking an elementary stream.
int frameBytes(const uint8_t* h) {
    static const uint32_t kBitrate[15] = {0,   32,  48,  56,  64,  80,  96, 112,
                                          128, 160, 192, 224, 256, 320, 384};
    static const uint32_t kRate[3] = {44100, 48000, 32000};
    const int bitrate = (h[2] >> 4) & 0x0F;
    const int rate = (h[2] >> 2) & 3;
    const int padding = (h[2] >> 1) & 1;
    if (bitrate == 0 || bitrate == 15 || rate == 3) {
        return 0;
    }
    return 144 * static_cast<int>(kBitrate[bitrate] * 1000u) / static_cast<int>(kRate[rate]) +
           padding;
}

bool isSync(const uint8_t* h) {
    return h[0] == 0xFF && (h[1] & 0xE0) == 0xE0;
}

// Decodes a raw MP2 elementary stream to interleaved float PCM. Used by the
// table-driven tests below and by the optional comparison dump.
std::vector<float> decodeElementary(const std::vector<uint8_t>& data, int* frame_count) {
    Decoder decoder;
    std::string error;
    if (!decoder.configure(nullptr, 0, error)) {
        return {};
    }
    std::vector<float> out;
    size_t pos = 0;
    int frames = 0;
    while (pos + 4 <= data.size()) {
        if (!isSync(data.data() + pos)) {
            ++pos;
            continue;
        }
        PcmBuffer pcm;
        if (decoder.decode(data.data() + pos, data.size() - pos, pcm, error) != Status::Ok) {
            break;
        }
        out.insert(out.end(), pcm.samples.begin(), pcm.samples.end());
        const int bytes = frameBytes(data.data() + pos);
        if (bytes <= 0) {
            break;
        }
        ++frames;
        pos += static_cast<size_t>(bytes);
    }
    if (frame_count != nullptr) {
        *frame_count = frames;
    }
    return out;
}

}  // namespace

TEST(Mp2Decoder, RejectsExtradata) {
    Decoder decoder;
    std::string error;
    const uint8_t config[2] = {0, 0};
    EXPECT_FALSE(decoder.configure(config, sizeof(config), error));
    EXPECT_FALSE(error.empty());
}

TEST(Mp2Decoder, RejectsNonLayerTwoFrames) {
    Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.configure(nullptr, 0, error));

    // A Layer III header (layer bits 01) must be refused rather than decoded
    // as Layer II.
    uint8_t frame[16] = {0xFF, 0xFB, 0x90, 0x00};
    PcmBuffer pcm;
    EXPECT_EQ(decoder.decode(frame, sizeof(frame), pcm, error), Status::Unsupported);

    // Garbage is not a frame at all.
    uint8_t garbage[16] = {0x00, 0x11, 0x22, 0x33};
    EXPECT_EQ(decoder.decode(garbage, sizeof(garbage), pcm, error), Status::Unsupported);
}

TEST(Mp2Decoder, RejectsTruncatedFrame) {
    Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.configure(nullptr, 0, error));
    // Layer II, 128 kbps, 44.1 kHz: the frame is 417 bytes, so four bytes are
    // far too few.
    uint8_t header[4] = {0xFF, 0xFD, 0x90, 0x00};
    PcmBuffer pcm;
    EXPECT_EQ(decoder.decode(header, sizeof(header), pcm, error), Status::Invalid);
}

// Decodes a real stream when one is pointed at, so the output can be diffed
// against a reference decoder. Skipped when the variable is unset.
TEST(Mp2Decoder, DecodesAnExternalStreamWhenProvided) {
    const char* src = std::getenv("MP2_DECODE_SRC");
    const char* dst = std::getenv("MP2_DECODE_OUT");
    if (src == nullptr || dst == nullptr) {
        GTEST_SKIP();
    }
    std::ifstream in(src, std::ios::binary);
    ASSERT_TRUE(in.good());
    const std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());

    int frames = 0;
    const std::vector<float> pcm = decodeElementary(data, &frames);
    ASSERT_GT(frames, 0);
    ASSERT_FALSE(pcm.empty());

    std::ofstream out(dst, std::ios::binary);
    out.write(reinterpret_cast<const char*>(pcm.data()),
              static_cast<std::streamsize>(pcm.size() * sizeof(float)));
    std::fprintf(stderr, "decoded %d frames, %zu samples\n", frames, pcm.size());
}
