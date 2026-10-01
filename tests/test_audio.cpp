#include "audio/AudioBuffer.hpp"
#include "audio/BitReader.hpp"
#include "audio/PcmGain.hpp"
#include "audio/SdlAudioSink.hpp"
#include "audio/Simd.hpp"
#include "audio/aac/AacConfig.hpp"
#include "audio/aac/AacDecoder.hpp"
#include "audio/aac/AacTables.hpp"
#include "audio/aac/Huffman.hpp"
#include "audio/aac/Mdct.hpp"
#include "audio/aac/Window.hpp"

#include <SDL3/SDL.h>

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using player::audio::BitReader;
using player::audio::Mute;
using player::audio::aac::AdtsHeader;
using player::audio::aac::AudioSpecificConfig;
using player::audio::aac::Decoder;
using player::audio::aac::Status;

// Writes bits MSB-first, mirroring the AAC syntax.
class BitWriter {
public:
    void write(uint32_t value, unsigned count) {
        for (unsigned i = 0; i < count; ++i) {
            const unsigned bit = (value >> (count - 1 - i)) & 1u;
            if ((bit_ & 7) == 0) {
                data_.push_back(0);
            }
            data_.back() = static_cast<uint8_t>(data_.back() | (bit << (7 - (bit_ & 7))));
            ++bit_;
        }
    }

    std::vector<uint8_t> bytes() const { return data_; }

private:
    std::vector<uint8_t> data_;
    size_t bit_ = 0;
};

enum class Crc {
    Absent,
    Present,
};

std::vector<uint8_t> makeAdts(uint8_t profile, uint8_t freq_index, uint8_t channel_config,
                              uint16_t frame_length, Crc crc, uint8_t raw_blocks) {
    const bool protection_absent = crc == Crc::Absent;
    std::vector<uint8_t> header(protection_absent ? 7 : 9, 0);
    header[0] = 0xFF;
    header[1] = static_cast<uint8_t>(protection_absent ? 0xF1 : 0xF0);
    header[2] = static_cast<uint8_t>((profile << 6) | ((freq_index & 0x0F) << 2) |
                                     ((channel_config >> 2) & 0x01));
    header[3] = static_cast<uint8_t>(((channel_config & 0x03) << 6) | ((frame_length >> 11) & 0x03));
    header[4] = static_cast<uint8_t>((frame_length >> 3) & 0xFF);
    header[5] = static_cast<uint8_t>(((frame_length & 0x07) << 5) | 0x1F);
    header[6] = static_cast<uint8_t>(0xFC | (raw_blocks & 0x03));
    return header;
}

// AudioSpecificConfig for AAC LC, 48 kHz stereo: 0x11 0x90.
const std::vector<uint8_t> kAscLc48kStereo = {0x11, 0x90};

}  // namespace

TEST(BitReader, ReadsMsbFirstAcrossBytes) {
    const uint8_t data[] = {0xB4, 0x2A};
    BitReader reader(data, sizeof(data));
    uint32_t value = 0;

    EXPECT_TRUE(reader.read(4, value));
    EXPECT_EQ(value, 0xBu);
    EXPECT_TRUE(reader.peek(4, value));
    EXPECT_EQ(value, 0x4u);
    EXPECT_TRUE(reader.read(8, value));
    EXPECT_EQ(value, 0x42u);
    EXPECT_TRUE(reader.read(4, value));
    EXPECT_EQ(value, 0xAu);
    EXPECT_EQ(reader.bitsLeft(), 0u);
    EXPECT_FALSE(reader.read(1, value));
}

TEST(BitReader, Reads32BitsAndSkips) {
    const uint8_t data[] = {0x12, 0x34, 0x56, 0x78, 0x9A};
    BitReader reader(data, sizeof(data));
    uint32_t value = 0;

    EXPECT_TRUE(reader.read(32, value));
    EXPECT_EQ(value, 0x12345678u);
    EXPECT_EQ(reader.bitPosition(), 32u);
    EXPECT_TRUE(reader.read(8, value));
    EXPECT_EQ(value, 0x9Au);

    BitReader skip_reader(data, sizeof(data));
    EXPECT_TRUE(skip_reader.skip(3));
    EXPECT_TRUE(skip_reader.read(5, value));
    EXPECT_EQ(value, 0x12u);
    EXPECT_FALSE(skip_reader.skip(100));
}

TEST(BitReader, AlignsToByteBoundary) {
    const uint8_t data[] = {0xA5, 0xFF};
    BitReader reader(data, sizeof(data));
    uint32_t value = 0;

    EXPECT_TRUE(reader.read(3, value));
    EXPECT_FALSE(reader.byteAligned());
    EXPECT_TRUE(reader.alignToByte());
    EXPECT_TRUE(reader.byteAligned());
    EXPECT_TRUE(reader.read(8, value));
    EXPECT_EQ(value, 0xFFu);
}

TEST(PcmGain, ClampsVolume) {
    EXPECT_FLOAT_EQ(player::audio::clampVolume(-0.5f), 0.0f);
    EXPECT_FLOAT_EQ(player::audio::clampVolume(2.0f), 1.0f);
    EXPECT_FLOAT_EQ(player::audio::clampVolume(0.25f), 0.25f);
}

TEST(PcmGain, MuteForcesSilence) {
    EXPECT_FLOAT_EQ(player::audio::volumeGain(0.8f, Mute::On), 0.0f);
    EXPECT_FLOAT_EQ(player::audio::volumeGain(0.8f, Mute::Off), 0.8f);
}

TEST(PcmGain, AppliesGainAndClamps) {
    float samples[] = {0.5f, -0.5f, 2.0f, -2.0f};
    player::audio::applyGain(samples, 4, 0.5f);
    EXPECT_FLOAT_EQ(samples[0], 0.25f);
    EXPECT_FLOAT_EQ(samples[1], -0.25f);
    EXPECT_FLOAT_EQ(samples[2], 1.0f);
    EXPECT_FLOAT_EQ(samples[3], -1.0f);

    player::audio::applyGain(samples, 4, 0.0f);
    EXPECT_FLOAT_EQ(samples[0], 0.0f);
    EXPECT_FLOAT_EQ(samples[3], 0.0f);
}

TEST(AacConfig, RejectsTruncatedAsc) {
    std::string error;
    AudioSpecificConfig config;
    const uint8_t data[] = {0x11};
    EXPECT_FALSE(player::audio::aac::parseAudioSpecificConfig(data, sizeof(data), config, &error));
    EXPECT_FALSE(error.empty());
}

TEST(AacConfig, ParsesLcStereoAsc) {
    AudioSpecificConfig config;
    std::string error;
    ASSERT_TRUE(player::audio::aac::parseAudioSpecificConfig(
        kAscLc48kStereo.data(), kAscLc48kStereo.size(), config, &error))
        << error;
    EXPECT_EQ(config.object_type, 2);
    EXPECT_EQ(config.sample_rate, 48000u);
    EXPECT_EQ(config.channel_config, 2);
    EXPECT_EQ(config.channel_count, 2u);
    EXPECT_FALSE(config.sbr_present);
}

TEST(AacConfig, ParsesExplicitSampleRate) {
    BitWriter writer;
    writer.write(2, 5);       // AAC LC
    writer.write(15, 4);      // explicit rate follows
    writer.write(44100, 24);  // sampling frequency
    writer.write(1, 4);       // mono
    writer.write(0, 3);       // GASpecificConfig
    const std::vector<uint8_t> asc = writer.bytes();

    AudioSpecificConfig config;
    std::string error;
    ASSERT_TRUE(player::audio::aac::parseAudioSpecificConfig(asc.data(), asc.size(), config, &error))
        << error;
    EXPECT_EQ(config.sample_rate, 44100u);
    EXPECT_EQ(config.channel_count, 1u);
}

TEST(AacConfig, DetectsSbr) {
    BitWriter writer;
    writer.write(5, 5);   // HE-AAC
    writer.write(6, 4);   // core rate index 6 = 24000
    writer.write(2, 4);   // stereo
    writer.write(3, 4);   // extension rate index 3 = 48000
    writer.write(2, 5);   // core object type = AAC LC
    writer.write(0, 3);   // GASpecificConfig
    const std::vector<uint8_t> asc = writer.bytes();

    AudioSpecificConfig config;
    std::string error;
    ASSERT_TRUE(player::audio::aac::parseAudioSpecificConfig(asc.data(), asc.size(), config, &error))
        << error;
    EXPECT_EQ(config.object_type, 2);
    EXPECT_EQ(config.sample_rate, 24000u);
    EXPECT_EQ(config.extension_sample_rate, 48000u);
    EXPECT_TRUE(config.sbr_present);
}

TEST(AacConfig, RejectsPceChannelConfig) {
    BitWriter writer;
    writer.write(2, 5);
    writer.write(3, 4);
    writer.write(0, 4);  // channel configuration 0 needs a PCE
    writer.write(0, 3);
    const std::vector<uint8_t> asc = writer.bytes();

    AudioSpecificConfig config;
    std::string error;
    EXPECT_FALSE(
        player::audio::aac::parseAudioSpecificConfig(asc.data(), asc.size(), config, &error));
    EXPECT_FALSE(error.empty());
}

TEST(AacConfig, ParsesAdtsHeader) {
    const std::vector<uint8_t> frame =
        makeAdts(1, 4, 2, 100, Crc::Absent, /*raw_blocks=*/0);
    AdtsHeader header;
    std::string error;
    ASSERT_TRUE(player::audio::aac::parseAdtsHeader(frame.data(), frame.size(), header, &error))
        << error;
    EXPECT_EQ(header.object_type, 2);
    EXPECT_EQ(header.sample_rate, 44100u);
    EXPECT_EQ(header.channel_config, 2);
    EXPECT_EQ(header.frame_length, 100);
    EXPECT_EQ(header.header_length, 7);
    EXPECT_EQ(header.raw_blocks, 0);
    EXPECT_TRUE(player::audio::aac::hasAdtsSyncword(frame.data(), frame.size()));
}

TEST(AacConfig, ParsesAdtsHeaderWithCrc) {
    const std::vector<uint8_t> frame =
        makeAdts(0, 3, 1, 50, Crc::Present, /*raw_blocks=*/1);
    AdtsHeader header;
    std::string error;
    ASSERT_TRUE(player::audio::aac::parseAdtsHeader(frame.data(), frame.size(), header, &error))
        << error;
    EXPECT_EQ(header.object_type, 1);
    EXPECT_EQ(header.sample_rate, 48000u);
    EXPECT_EQ(header.header_length, 9);
    EXPECT_EQ(header.raw_blocks, 1);
}

TEST(AacConfig, RejectsBadAdtsHeader) {
    AdtsHeader header;
    std::string error;
    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    EXPECT_FALSE(player::audio::aac::parseAdtsHeader(garbage, sizeof(garbage), header, &error));

    const std::vector<uint8_t> too_short = makeAdts(1, 4, 2, 3, Crc::Absent, 0);
    EXPECT_FALSE(
        player::audio::aac::parseAdtsHeader(too_short.data(), too_short.size(), header, &error));
}

TEST(AacDecoder, ConfiguresFromAsc) {
    Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.configure(kAscLc48kStereo.data(), kAscLc48kStereo.size(), error)) << error;
    EXPECT_TRUE(decoder.configured());
    EXPECT_EQ(decoder.config().sample_rate, 48000u);
    EXPECT_EQ(decoder.config().channel_count, 2u);
}

TEST(AacDecoder, RejectsHeAac) {
    BitWriter writer;
    writer.write(5, 5);
    writer.write(3, 4);
    writer.write(2, 4);
    writer.write(3, 4);
    writer.write(2, 5);
    writer.write(0, 3);
    const std::vector<uint8_t> asc = writer.bytes();

    Decoder decoder;
    std::string error;
    EXPECT_FALSE(decoder.configure(asc.data(), asc.size(), error));
    EXPECT_FALSE(error.empty());
}

TEST(AacDecoder, RejectsRawFrameWithoutConfig) {
    Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.configure(nullptr, 0, error));
    const uint8_t frame[] = {0x00, 0x01, 0x02};
    player::audio::PcmBuffer pcm;
    EXPECT_EQ(decoder.decode(frame, sizeof(frame), pcm, error), Status::Invalid);
}

TEST(AacDecoder, RejectsGarbagePayload) {
    Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.configure(nullptr, 0, error));

    std::vector<uint8_t> frame = makeAdts(1, 4, 2, 20, Crc::Absent, 0);
    frame.resize(20, 0);
    player::audio::PcmBuffer pcm;
    EXPECT_EQ(decoder.decode(frame.data(), frame.size(), pcm, error), Status::Invalid);
    EXPECT_FALSE(error.empty());
}

TEST(AacDecoder, RejectsTruncatedAdts) {
    Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.configure(nullptr, 0, error));
    const std::vector<uint8_t> frame = makeAdts(1, 4, 2, 200, Crc::Absent, 0);
    player::audio::PcmBuffer pcm;
    EXPECT_EQ(decoder.decode(frame.data(), frame.size(), pcm, error), Status::Invalid);
    EXPECT_FALSE(error.empty());
}

TEST(Mdct, MatchesDirectTransform) {
    for (const size_t bins : {4u, 16u, 64u}) {
        player::audio::aac::Mdct mdct(bins);
        std::vector<float> spectrum(bins);
        std::vector<float> fast(2 * bins);
        std::vector<float> direct(2 * bins);

        uint32_t state = 0x12345678u;
        for (float& value : spectrum) {
            state = state * 1664525u + 1013904223u;
            value = static_cast<float>((state >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        }

        mdct.inverse(spectrum.data(), fast.data());
        mdct.inverseDirect(spectrum.data(), direct.data());
        for (size_t i = 0; i < fast.size(); ++i) {
            EXPECT_NEAR(fast[i], direct[i], 1e-4f) << "bins=" << bins << " i=" << i;
        }
    }
}

TEST(Window, SineSatisfiesPrincenBradley) {
    for (const size_t bins : {8u, 128u, 1024u}) {
        const std::vector<float> window = player::audio::aac::sineWindow(bins);
        ASSERT_EQ(window.size(), 2 * bins);
        for (size_t i = 0; i < bins; ++i) {
            const float sum = window[i] * window[i] + window[i + bins] * window[i + bins];
            EXPECT_NEAR(sum, 1.0f, 1e-5f) << "bins=" << bins << " i=" << i;
        }
    }
}

TEST(Window, KbdSatisfiesPrincenBradley) {
    for (const float alpha : {4.0f, 6.0f}) {
        const size_t bins = 128;
        const std::vector<float> window = player::audio::aac::kbdWindow(bins, alpha);
        ASSERT_EQ(window.size(), 2 * bins);
        for (size_t i = 0; i < bins; ++i) {
            const float sum = window[i] * window[i] + window[i + bins] * window[i + bins];
            EXPECT_NEAR(sum, 1.0f, 1e-4f) << "alpha=" << alpha << " i=" << i;
        }
        for (size_t i = 0; i < window.size(); ++i) {
            EXPECT_FLOAT_EQ(window[i], window[window.size() - 1 - i]);
        }
    }
}

TEST(SdlAudioSink, BoundsQueueAndCountsFrames) {
    SDL_setenv_unsafe("SDL_AUDIODRIVER", "dummy", 1);

    player::audio::SdlAudioSink sink;
    player::audio::AudioFormat format;
    format.channels = 2;
    format.sample_rate = 48000;
    sink.configure(format);

    const std::vector<float> samples(48000 * 2 * 3, 0.0f);  // 3 seconds, cap is 1
    sink.push(samples.data(), 48000 * 3);

    const uint64_t consumed = sink.framesConsumed();
    EXPECT_GT(consumed, 0u);
    EXPECT_LT(consumed, 48000u * 3);
    EXPECT_EQ(consumed + sink.framesDropped(), 48000u * 3);
    EXPECT_LE(sink.queuedMs(), 1100);

    sink.reset();
    EXPECT_EQ(sink.framesConsumed(), 0u);
    EXPECT_EQ(sink.framesDropped(), 0u);
}

TEST(Huffman, CodebooksRoundTrip) {
    for (unsigned book = 0; book < 12; ++book) {
        const player::audio::aac::HuffmanCodebook& codebook =
            player::audio::aac::huffmanCodebook(book);
        const player::audio::aac::HuffmanTable& table = player::audio::aac::huffmanTable(book);
        ASSERT_TRUE(table.valid());
        for (unsigned symbol = 0; symbol < codebook.entry_count; ++symbol) {
            const player::audio::aac::HuffmanEntry& entry = codebook.entries[symbol];
            BitWriter writer;
            writer.write(entry.code, entry.bits);
            const std::vector<uint8_t> bytes = writer.bytes();

            BitReader reader(bytes.data(), bytes.size());
            int8_t values[4] = {};
            unsigned dimension = 0;
            ASSERT_TRUE(table.decode(reader, values, dimension))
                << "book " << book << " symbol " << symbol;
            EXPECT_EQ(dimension, codebook.dimension);
            for (unsigned i = 0; i < dimension; ++i) {
                EXPECT_EQ(values[i], entry.values[i]) << "book " << book << " symbol " << symbol;
            }
        }
    }
}

TEST(Window, KbdMatchesIsoReference) {
    const std::vector<float> window = player::audio::aac::kbdWindow(1024, 4.0f);
    // Reference values from the ISO/IEC 14496-3 KBD table (alpha = 4).
    EXPECT_NEAR(window[0], 0.00029256153896361f, 1e-9f);
    EXPECT_NEAR(window[511], 0.70611933911096f, 1e-6f);
    EXPECT_NEAR(window[1023], 0.99999995720387f, 1e-6f);
}

TEST(AacDecoder, DecodesReferenceAdtsStream) {
    // Two ADTS frames of a 440 Hz sine (48 kHz mono, 64 kbps) produced by the
    // reference encoder. The expected samples are the ISO decoder output.
    const std::vector<uint8_t> stream = {
        0xff, 0xf1, 0x4c, 0x40, 0x18, 0xff, 0xfc, 0xde, 0x02, 0x00, 0x4c, 0x61,
        0x76, 0x63, 0x36, 0x32, 0x2e, 0x31, 0x31, 0x2e, 0x31, 0x30, 0x30, 0x00,
        0x02, 0x68, 0xaa, 0x59, 0xe9, 0xf0, 0x4d, 0x0b, 0x0b, 0x5a, 0x99, 0x7f,
        0xb7, 0xdf, 0xad, 0xea, 0xfc, 0xde, 0xb7, 0x72, 0xd2, 0xab, 0x55, 0x24,
        0x77, 0x54, 0xd4, 0x92, 0x24, 0x08, 0x26, 0xe9, 0x86, 0xe6, 0xc5, 0x69,
        0xeb, 0x67, 0xb2, 0xae, 0x90, 0x3a, 0xbb, 0x82, 0x59, 0x26, 0x66, 0xe1,
        0x01, 0xcf, 0x5e, 0x35, 0x4b, 0x49, 0xa2, 0xb8, 0xcf, 0x7a, 0x2f, 0x5e,
        0xe2, 0x75, 0xae, 0xd3, 0xd1, 0xb8, 0x1d, 0xbb, 0xcf, 0x7b, 0x17, 0xa4,
        0xf2, 0xab, 0x8d, 0x9a, 0xb3, 0x3d, 0x89, 0xb9, 0x59, 0x6e, 0x56, 0x5b,
        0x94, 0xed, 0x6b, 0x1b, 0x8a, 0xb8, 0xd9, 0xae, 0x36, 0x6a, 0xcc, 0xf4,
        0xed, 0x6a, 0xcb, 0x6a, 0x9d, 0xad, 0x4e, 0xd6, 0x9f, 0x5b, 0x3e, 0xb6,
        0x7d, 0x6c, 0xa5, 0x52, 0x95, 0xc6, 0x57, 0x35, 0x7e, 0x6a, 0xfc, 0xa5,
        0xb3, 0xeb, 0x68, 0xad, 0x45, 0x6a, 0x6c, 0x66, 0x63, 0x33, 0x19, 0x99,
        0xd1, 0x5a, 0x8a, 0xd4, 0x56, 0xa2, 0xce, 0x9b, 0x19, 0x98, 0xcc, 0xc6,
        0x64, 0xa8, 0xad, 0x45, 0x6a, 0x2b, 0x51, 0x4a, 0x26, 0x33, 0x31, 0xa4,
        0x9a, 0x4a, 0x1e, 0x89, 0x26, 0x91, 0xaa, 0x6a, 0x9a, 0x49, 0xa4, 0xa2,
        0x4a, 0x1e, 0x89, 0x26, 0x91, 0xaa, 0x6e, 0xff, 0xf1, 0x4c, 0x40, 0x1e,
        0x5f, 0xfc, 0x01, 0x34, 0x98, 0xda, 0xd9, 0x3d, 0xd6, 0x50, 0xd3, 0x4c,
        0x26, 0xca, 0x1a, 0x6c, 0x4b, 0x59, 0x8a, 0x99, 0xed, 0xf5, 0x7a, 0xb6,
        0xd6, 0xae, 0x5b, 0xff, 0xc3, 0x8f, 0xf3, 0xec, 0x17, 0xab, 0xbf, 0xff,
        0xb1, 0xc7, 0xfb, 0xf4, 0x17, 0xab, 0x9f, 0xff, 0x6f, 0xe3, 0xf5, 0xe2,
        0x0b, 0xbd, 0x58, 0xda, 0x9d, 0x9a, 0xb0, 0x3a, 0x01, 0xf4, 0xbc, 0xf3,
        0x28, 0xeb, 0x2c, 0xf7, 0xf8, 0xf2, 0x1c, 0x74, 0x13, 0xd5, 0x37, 0x3c,
        0xc6, 0x99, 0x6a, 0x71, 0x31, 0x53, 0xb5, 0xa7, 0xd4, 0xee, 0x9b, 0x35,
        0x54, 0xa5, 0xdb, 0xaf, 0xf9, 0x3f, 0x19, 0xf9, 0x4e, 0x89, 0xbd, 0x78,
        0x24, 0x24, 0x24, 0x26, 0xf6, 0x61, 0x21, 0x21, 0x27, 0x1d, 0xc2, 0x42,
        0x42, 0x4e, 0x4c, 0xb7, 0x77, 0x09, 0x09, 0x09, 0x38, 0xb6, 0x9d, 0xdc,
        0x24, 0x24, 0xa8, 0x4c, 0xd9, 0x3b, 0xbf, 0xf2, 0xf1, 0xfc, 0x30, 0xec,
        0x94, 0x41, 0x46, 0xe8, 0x43, 0x9d, 0xeb, 0xe1, 0x2c, 0x9d, 0x0d, 0xd4,
        0xb1, 0x89, 0x6d, 0x69, 0x86, 0x80, 0x9b, 0x89, 0x9e, 0xa5, 0x7e, 0x82,
        0x4a, 0xd5, 0xb2, 0x92, 0x86, 0x17, 0x0c, 0x70, 0x69, 0x26, 0x76, 0x67,
        0x37, 0xf9, 0x7c, 0xbe, 0x5f, 0x27, 0x9e, 0xf9, 0x7c, 0xb8, 0x93, 0x07,
        0xcb, 0xe5, 0xf2, 0x8a, 0x22, 0x00, 0x7c, 0xbe, 0x5f, 0x2e, 0x31, 0x44,
        0x00, 0xf9, 0x7c, 0xb2, 0xc4, 0x40, 0x01, 0x3c, 0x4a, 0x50, 0x11, 0x44,
        0x00, 0x6f, 0x5d, 0x79, 0x7e, 0xb8, 0x9e, 0xeb, 0x17, 0x75, 0x8b, 0x55,
        0x52, 0xd3, 0x2d, 0x32, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12, 0x12,
        0x12, 0x70, 0x24, 0x24, 0xe0, 0x4d, 0xd1, 0x29, 0x78,
    };

    player::audio::aac::Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.configure(nullptr, 0, error)) << error;

    std::vector<float> samples;
    size_t offset = 0;
    while (offset + 7 <= stream.size()) {
        player::audio::aac::AdtsHeader header;
        ASSERT_TRUE(player::audio::aac::parseAdtsHeader(
            stream.data() + offset, stream.size() - offset, header, &error))
            << error;
        player::audio::PcmBuffer pcm;
        ASSERT_EQ(decoder.decode(stream.data() + offset, header.frame_length, pcm, error),
                  Status::Ok)
            << error;
        EXPECT_EQ(pcm.channels, 1u);
        EXPECT_EQ(pcm.sample_rate, 48000u);
        samples.insert(samples.end(), pcm.samples.begin(), pcm.samples.end());
        offset += header.frame_length;
    }

    ASSERT_EQ(samples.size(), 2u * 1024u);
    EXPECT_NEAR(samples[1024 + 100], -0.0634584f, 1e-5f);
    EXPECT_NEAR(samples[1024 + 500], -0.0622742f, 1e-5f);
    EXPECT_NEAR(samples[1024 + 1000], 0.1065561f, 1e-5f);
    EXPECT_NEAR(samples[1024 + 1023], 0.0816464f, 1e-5f);
}

TEST(AacDecoder, DecodesRawFrameWithAsc) {
    // Same stream, but the first frame is fed as a raw access unit using an
    // explicit AudioSpecificConfig (AAC LC, 48 kHz, mono).
    const uint8_t asc[] = {0x11, 0x88};
    const std::vector<uint8_t> frame = {
        0xff, 0xf1, 0x4c, 0x40, 0x18, 0xff, 0xfc, 0xde, 0x02, 0x00, 0x4c, 0x61,
        0x76, 0x63, 0x36, 0x32, 0x2e, 0x31, 0x31, 0x2e, 0x31, 0x30, 0x30, 0x00,
        0x02, 0x68, 0xaa, 0x59, 0xe9, 0xf0, 0x4d, 0x0b, 0x0b, 0x5a, 0x99, 0x7f,
        0xb7, 0xdf, 0xad, 0xea, 0xfc, 0xde, 0xb7, 0x72, 0xd2, 0xab, 0x55, 0x24,
        0x77, 0x54, 0xd4, 0x92, 0x24, 0x08, 0x26, 0xe9, 0x86, 0xe6, 0xc5, 0x69,
        0xeb, 0x67, 0xb2, 0xae, 0x90, 0x3a, 0xbb, 0x82, 0x59, 0x26, 0x66, 0xe1,
        0x01, 0xcf, 0x5e, 0x35, 0x4b, 0x49, 0xa2, 0xb8, 0xcf, 0x7a, 0x2f, 0x5e,
        0xe2, 0x75, 0xae, 0xd3, 0xd1, 0xb8, 0x1d, 0xbb, 0xcf, 0x7b, 0x17, 0xa4,
        0xf2, 0xab, 0x8d, 0x9a, 0xb3, 0x3d, 0x89, 0xb9, 0x59, 0x6e, 0x56, 0x5b,
        0x94, 0xed, 0x6b, 0x1b, 0x8a, 0xb8, 0xd9, 0xae, 0x36, 0x6a, 0xcc, 0xf4,
        0xed, 0x6a, 0xcb, 0x6a, 0x9d, 0xad, 0x4e, 0xd6, 0x9f, 0x5b, 0x3e, 0xb6,
        0x7d, 0x6c, 0xa5, 0x52, 0x95, 0xc6, 0x57, 0x35, 0x7e, 0x6a, 0xfc, 0xa5,
        0xb3, 0xeb, 0x68, 0xad, 0x45, 0x6a, 0x6c, 0x66, 0x63, 0x33, 0x19, 0x99,
        0xd1, 0x5a, 0x8a, 0xd4, 0x56, 0xa2, 0xce, 0x9b, 0x19, 0x98, 0xcc, 0xc6,
        0x64, 0xa8, 0xad, 0x45, 0x6a, 0x2b, 0x51, 0x4a, 0x26, 0x33, 0x31, 0xa4,
        0x9a, 0x4a, 0x1e, 0x89, 0x26, 0x91, 0xaa, 0x6a, 0x9a, 0x49, 0xa4, 0xa2,
        0x4a, 0x1e, 0x89, 0x26, 0x91, 0xaa, 0x6e,
    };

    player::audio::aac::Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.configure(asc, sizeof(asc), error)) << error;

    player::audio::aac::AdtsHeader header;
    ASSERT_TRUE(player::audio::aac::parseAdtsHeader(frame.data(), frame.size(), header, &error))
        << error;
    const std::vector<uint8_t> raw(frame.begin() + header.header_length,
                                   frame.begin() + header.frame_length);

    player::audio::PcmBuffer pcm;
    ASSERT_EQ(decoder.decode(raw.data(), raw.size(), pcm, error), Status::Ok) << error;
    ASSERT_EQ(pcm.samples.size(), 1024u);
    EXPECT_NEAR(pcm.samples[100], 0.0003602f, 1e-6f);
    EXPECT_NEAR(pcm.samples[500], -0.0005428f, 1e-6f);
    EXPECT_NEAR(pcm.samples[1000], 0.0009346f, 1e-6f);
}

namespace {

player::audio::PcmBuffer makeChunk(int64_t pts_ms, uint32_t rate, size_t frames) {
    player::audio::PcmBuffer chunk;
    chunk.channels = 1;
    chunk.sample_rate = rate;
    chunk.pts = pts_ms;
    chunk.samples.assign(frames, 0.25f);
    return chunk;
}

}  // namespace

TEST(AudioBuffer, PreservesOrderAndTimestamps) {
    player::audio::AudioBuffer buffer(1000);
    buffer.push(makeChunk(0, 1000, 250));    // 250 ms
    buffer.push(makeChunk(250, 1000, 250));  // 250 ms

    EXPECT_FALSE(buffer.empty());
    EXPECT_EQ(buffer.chunks(), 2u);
    EXPECT_EQ(buffer.durationMs(), 500);
    EXPECT_EQ(buffer.frontPtsMs(), 0);
    EXPECT_EQ(buffer.frontEndPtsMs(), 250);
    EXPECT_EQ(buffer.endPtsMs(), 500);

    player::audio::PcmBuffer out;
    ASSERT_TRUE(buffer.pop(out));
    EXPECT_EQ(out.pts, 0);
    ASSERT_TRUE(buffer.pop(out));
    EXPECT_EQ(out.pts, 250);
    EXPECT_FALSE(buffer.pop(out));
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.durationMs(), 0);
}

TEST(AudioBuffer, DropsOldestWhenFull) {
    player::audio::AudioBuffer buffer(500);
    buffer.push(makeChunk(0, 1000, 250));
    buffer.push(makeChunk(250, 1000, 250));
    buffer.push(makeChunk(500, 1000, 250));  // exceeds 500 ms: oldest is dropped

    EXPECT_EQ(buffer.durationMs(), 500);
    EXPECT_EQ(buffer.frontPtsMs(), 250);
    EXPECT_EQ(buffer.endPtsMs(), 750);
    EXPECT_EQ(buffer.droppedChunks(), 1u);

    buffer.clear();
    EXPECT_TRUE(buffer.empty());
    EXPECT_EQ(buffer.droppedChunks(), 0u);
}

TEST(Simd, Pow34TableValues) {
    const float* table = player::audio::simd::pow34Table();
    ASSERT_NE(table, nullptr);
    EXPECT_FLOAT_EQ(table[0], 0.0f);
    EXPECT_FLOAT_EQ(table[8], 16.0f);
    EXPECT_NEAR(table[8191], std::pow(8191.0, 4.0 / 3.0), 1.0);
}

TEST(Simd, GainMatchesScalar) {
    if (!player::audio::simd::hasAvx2()) {
        GTEST_SKIP() << "AVX2 not available";
    }
    uint32_t state = 0x12345678u;
    std::vector<float> scalar(999);
    for (float& value : scalar) {
        state = state * 1664525u + 1013904223u;
        value = static_cast<float>((state >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        value *= 2.0f;  // exercise clipping
    }
    std::vector<float> avx2 = scalar;
    player::audio::simd::applyGainScalar(scalar.data(), scalar.size(), 0.37f);
    player::audio::simd::applyGainAvx2(avx2.data(), avx2.size(), 0.37f);
    for (size_t i = 0; i < scalar.size(); ++i) {
        EXPECT_FLOAT_EQ(scalar[i], avx2[i]) << "i=" << i;
    }
}

TEST(Simd, DequantizeMatchesScalar) {
    if (!player::audio::simd::hasAvx2()) {
        GTEST_SKIP() << "AVX2 not available";
    }
    std::vector<int16_t> quant(1000);
    uint32_t state = 0x9E3779B9u;
    for (size_t i = 0; i < quant.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        const int mode = static_cast<int>((state >> 16) & 7);
        if (mode == 0) {
            quant[i] = 0;
        } else if (mode == 1) {
            quant[i] = 16;
        } else if (mode == 2) {
            quant[i] = -8191;
        } else {
            quant[i] = static_cast<int16_t>((state >> 8) & 0x3FF) - 512;
        }
    }
    std::vector<float> scalar(quant.size());
    std::vector<float> avx2(quant.size());
    player::audio::simd::dequantizeBandScalar(quant.data(), scalar.data(), quant.size(), 3.5f);
    player::audio::simd::dequantizeBandAvx2(quant.data(), avx2.data(), quant.size(), 3.5f);
    for (size_t i = 0; i < quant.size(); ++i) {
        EXPECT_FLOAT_EQ(scalar[i], avx2[i]) << "i=" << i;
    }
}

TEST(Simd, WindowAndScaleMatchScalar) {
    if (!player::audio::simd::hasAvx2()) {
        GTEST_SKIP() << "AVX2 not available";
    }
    uint32_t state = 0xC0FFEEu;
    std::vector<float> block(777);
    std::vector<float> window(777);
    for (size_t i = 0; i < block.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        block[i] = static_cast<float>((state >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        state = state * 1664525u + 1013904223u;
        window[i] = static_cast<float>((state >> 8) & 0xFFFF) / 65536.0f;
    }

    std::vector<float> a = block;
    std::vector<float> b = block;
    player::audio::simd::windowMultiplyScalar(a.data(), window.data(), a.size());
    player::audio::simd::windowMultiplyAvx2(b.data(), window.data(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(a[i], b[i]) << "i=" << i;
    }

    a = block;
    b = block;
    player::audio::simd::addWindowedScalar(a.data(), window.data(), window.data(), a.size());
    player::audio::simd::addWindowedAvx2(b.data(), window.data(), window.data(), b.size());
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(a[i], b[i]) << "i=" << i;
    }

    a = block;
    b = block;
    player::audio::simd::scaleCopyScalar(a.data(), block.data(), a.size(), -1.75f);
    player::audio::simd::scaleCopyAvx2(b.data(), block.data(), b.size(), -1.75f);
    for (size_t i = 0; i < a.size(); ++i) {
        EXPECT_FLOAT_EQ(a[i], b[i]) << "i=" << i;
    }
}

TEST(Simd, MidSideMatchesScalar) {
    if (!player::audio::simd::hasAvx2()) {
        GTEST_SKIP() << "AVX2 not available";
    }
    uint32_t state = 0xBEEF1234u;
    std::vector<float> left(555);
    std::vector<float> right(555);
    for (size_t i = 0; i < left.size(); ++i) {
        state = state * 1664525u + 1013904223u;
        left[i] = static_cast<float>((state >> 8) & 0xFFFF) / 32768.0f - 1.0f;
        state = state * 1664525u + 1013904223u;
        right[i] = static_cast<float>((state >> 8) & 0xFFFF) / 32768.0f - 1.0f;
    }
    std::vector<float> left_scalar = left;
    std::vector<float> right_scalar = right;
    std::vector<float> left_avx2 = left;
    std::vector<float> right_avx2 = right;
    player::audio::simd::midSideScalar(left_scalar.data(), right_scalar.data(), left.size());
    player::audio::simd::midSideAvx2(left_avx2.data(), right_avx2.data(), left.size());
    for (size_t i = 0; i < left.size(); ++i) {
        EXPECT_FLOAT_EQ(left_scalar[i], left_avx2[i]) << "i=" << i;
        EXPECT_FLOAT_EQ(right_scalar[i], right_avx2[i]) << "i=" << i;
    }
}

TEST(Simd, DftMatchesScalar) {
    if (!player::audio::simd::hasAvx2()) {
        GTEST_SKIP() << "AVX2 not available";
    }
    for (const size_t size : {256u, 2048u}) {
        uint32_t state = 0xDEADBEEFu;
        std::vector<double> input(2 * size);
        for (double& value : input) {
            state = state * 1664525u + 1013904223u;
            value = static_cast<double>((state >> 8) & 0xFFFF) / 32768.0 - 1.0;
        }
        std::vector<double> scalar = input;
        std::vector<double> avx2 = input;
        player::audio::simd::dftScalar(scalar.data(), size);
        player::audio::simd::dftAvx2(avx2.data(), size);
        for (size_t i = 0; i < scalar.size(); ++i) {
            EXPECT_NEAR(scalar[i], avx2[i], 1e-8) << "size=" << size << " i=" << i;
        }
    }
}
