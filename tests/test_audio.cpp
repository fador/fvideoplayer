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
#include "audio/opus/CeltEnergy.hpp"
#include "audio/opus/CeltPvq.hpp"
#include "audio/opus/OpusDecoder.hpp"
#include "audio/opus/OpusHead.hpp"
#include "audio/opus/OpusPacket.hpp"
#include "audio/opus/OpusTables.hpp"
#include "audio/opus/RangeDecoder.hpp"

#include <SDL3/SDL.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <utility>
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

TEST(AacDecoder, DecodesTnsOddOrderFrames) {
    // First two ADTS frames of a Media Foundation-encoded stereo file
    // (48 kHz, 128 kbps). The second frame uses TNS filters with odd orders;
    // a reflection-to-LPC conversion that updated the middle coefficient
    // twice made those filters unstable. Expected samples are the reference
    // decoder output.
    const std::vector<uint8_t> stream = {
        0xff, 0xf1, 0x4c, 0x80, 0x36, 0x3f, 0xfc, 0x21, 0x7a, 0x08, 0x18, 0x01,
        0xff, 0xff, 0xbc, 0x7d, 0xb9, 0x28, 0xc1, 0x42, 0x28, 0x5d, 0x9b, 0xb3,
        0x87, 0x1d, 0x71, 0xed, 0x3c, 0xe7, 0x40, 0x10, 0x02, 0x50, 0x0e, 0x6c,
        0xfd, 0x4f, 0x69, 0x2f, 0x3a, 0xfd, 0x31, 0x20, 0x0c, 0x91, 0xda, 0x48,
        0xb6, 0x49, 0x97, 0x50, 0x44, 0x31, 0x88, 0x98, 0x9f, 0x7d, 0xfa, 0x8f,
        0xd4, 0x7e, 0xa3, 0xf7, 0xdf, 0x5d, 0xe2, 0xdd, 0x1b, 0xeb, 0xbc, 0x5b,
        0x98, 0x69, 0x9e, 0x2d, 0xcc, 0x34, 0xcc, 0x16, 0x99, 0x82, 0xb4, 0xa5,
        0x74, 0xb4, 0xa5, 0x14, 0x14, 0x15, 0x0b, 0xe1, 0x8c, 0x79, 0x73, 0xda,
        0xa0, 0x70, 0xe4, 0x3d, 0x4c, 0x0b, 0x71, 0x6f, 0x26, 0x32, 0x6b, 0x3c,
        0xbd, 0x69, 0x8e, 0x8a, 0x63, 0xc5, 0x7c, 0xa2, 0xa9, 0x6d, 0x33, 0x9c,
        0xbf, 0xa4, 0xef, 0x32, 0xaa, 0x76, 0xe7, 0xbe, 0xd7, 0x96, 0xf7, 0x60,
        0x96, 0xf3, 0x03, 0xd6, 0x83, 0x2f, 0x57, 0x7f, 0x95, 0xfb, 0x36, 0x7f,
        0x15, 0xf9, 0x94, 0xe7, 0x6e, 0x05, 0x04, 0x5f, 0x04, 0xa9, 0x24, 0x94,
        0x28, 0xdb, 0x94, 0x2c, 0x6b, 0x1a, 0x79, 0xd4, 0x5e, 0xb3, 0x43, 0xd6,
        0x46, 0x72, 0x8d, 0xfe, 0x49, 0x64, 0xb6, 0x13, 0xa9, 0x57, 0x7b, 0x92,
        0x02, 0x19, 0x25, 0xaa, 0xe8, 0xc2, 0x10, 0x6d, 0xdc, 0x1a, 0x0a, 0x8f,
        0xff, 0xa6, 0xa6, 0x5c, 0x4a, 0x7d, 0x5b, 0xea, 0xea, 0x21, 0x6e, 0xb2,
        0xc2, 0xdf, 0x84, 0xb7, 0xd6, 0xab, 0x57, 0x86, 0xe1, 0x5e, 0xe0, 0x95,
        0x6a, 0x2f, 0xa2, 0x58, 0xfb, 0x33, 0x10, 0x44, 0x2d, 0xae, 0x8a, 0x88,
        0x10, 0xa2, 0x54, 0xca, 0x7a, 0xde, 0x3d, 0x0f, 0x3f, 0x5f, 0x17, 0xc4,
        0xeb, 0x40, 0x20, 0x25, 0x1c, 0xf4, 0xa0, 0xe5, 0xc4, 0x78, 0xd7, 0xee,
        0x3f, 0xf1, 0x22, 0x11, 0xf1, 0xaf, 0xac, 0xec, 0x5e, 0xe1, 0xe3, 0x5e,
        0xd5, 0xef, 0x19, 0x84, 0xc6, 0x2f, 0xae, 0xe5, 0x51, 0x11, 0x8d, 0x94,
        0x97, 0xaa, 0x56, 0x40, 0xcd, 0x53, 0xd3, 0xab, 0x8d, 0x58, 0xa3, 0x95,
        0x65, 0x51, 0xc5, 0x1e, 0xd6, 0xcb, 0x89, 0xc4, 0xe5, 0x38, 0x99, 0xd8,
        0xdb, 0x0b, 0x52, 0x41, 0x88, 0x2b, 0xe9, 0x47, 0x6a, 0xb9, 0x59, 0xb2,
        0x86, 0xca, 0xd9, 0x3c, 0x8f, 0x24, 0xe7, 0xa8, 0x7b, 0x9e, 0xba, 0x48,
        0xed, 0x6a, 0x22, 0xbc, 0x14, 0xe7, 0xd3, 0x65, 0xfb, 0x75, 0x54, 0x9c,
        0x1b, 0xdf, 0xa8, 0x33, 0x20, 0xdc, 0x27, 0xed, 0xec, 0x85, 0xe0, 0xa8,
        0xb8, 0x5a, 0xba, 0xfd, 0x4f, 0x89, 0x67, 0xd8, 0x13, 0x23, 0x16, 0xad,
        0x7d, 0x68, 0x8a, 0x93, 0x62, 0x42, 0xdb, 0x0d, 0x41, 0xea, 0x07, 0xa7,
        0x72, 0xe9, 0xaf, 0x8a, 0x31, 0xf6, 0xe0, 0xa9, 0x11, 0x67, 0xdc, 0xd8,
        0x76, 0xd0, 0x7e, 0x47, 0x47, 0xfc, 0x52, 0x26, 0x19, 0x8e, 0x72, 0x88,
        0x8c, 0xe6, 0x2d, 0xce, 0x67, 0x62, 0xbb, 0xb9, 0x4e, 0x0e, 0x25, 0xfe,
        0x46, 0x4b, 0x92, 0x6a, 0xd7, 0x77, 0xb2, 0x77, 0x98, 0xe5, 0x32, 0x5c,
        0x43, 0x8e, 0xb1, 0xcd, 0x4c, 0x65, 0x48, 0x76, 0x65, 0x5f, 0x40, 0x84,
        0x7f, 0xff, 0xf1, 0x4c, 0x80, 0x37, 0xdf, 0xfc, 0x21, 0x0a, 0x08, 0x00,
        0x00, 0x1f, 0xff, 0xbc, 0x3d, 0x9d, 0x88, 0x8b, 0x63, 0x21, 0x94, 0x28,
        0x56, 0x1f, 0x91, 0xf1, 0xf7, 0xfa, 0xb6, 0x8d, 0xf0, 0x05, 0x2c, 0xa9,
        0x57, 0x4a, 0x5b, 0x6d, 0x68, 0x07, 0x94, 0x05, 0x0f, 0xf3, 0xb5, 0xdc,
        0xe5, 0x81, 0x51, 0xba, 0xca, 0x4d, 0x0e, 0x0d, 0x4a, 0xb7, 0x9c, 0x4b,
        0x84, 0xee, 0x2a, 0x65, 0xd9, 0x48, 0x51, 0x11, 0xd6, 0x12, 0xbf, 0x86,
        0x13, 0x49, 0xbe, 0xa4, 0x4a, 0x5a, 0x91, 0xf4, 0x89, 0x95, 0x5b, 0x03,
        0x7c, 0xf0, 0x39, 0x78, 0x71, 0xbf, 0x8e, 0x93, 0xea, 0xf9, 0x5f, 0xbb,
        0x5d, 0xba, 0x58, 0xef, 0x02, 0x30, 0x6a, 0xa9, 0xae, 0x77, 0xc7, 0x09,
        0xbb, 0x71, 0x92, 0x69, 0x29, 0xc3, 0x43, 0xb5, 0x6c, 0xec, 0xec, 0xed,
        0x22, 0x01, 0xbd, 0xf5, 0xb3, 0xe2, 0xed, 0x23, 0x12, 0x56, 0x15, 0xe2,
        0xed, 0x26, 0x2e, 0xc7, 0x7b, 0xb3, 0xb5, 0x73, 0xbb, 0x5a, 0xc3, 0xb1,
        0xa3, 0xa1, 0x81, 0xba, 0x94, 0x4c, 0x87, 0x16, 0x8b, 0xe8, 0xcf, 0xa1,
        0xb8, 0x93, 0xf0, 0x57, 0x8c, 0xa9, 0x98, 0xb8, 0xb5, 0x07, 0xf5, 0x59,
        0x4a, 0x8d, 0x2c, 0xae, 0x0e, 0xfe, 0x9d, 0xb5, 0x14, 0x07, 0x60, 0x09,
        0xf5, 0x25, 0xa1, 0xd8, 0xf3, 0x0f, 0x92, 0xff, 0x0f, 0xc7, 0x85, 0xd5,
        0x43, 0x33, 0x7b, 0xda, 0x6d, 0x61, 0x9f, 0x4f, 0x13, 0x6f, 0xbb, 0x54,
        0x7e, 0x52, 0xee, 0x6c, 0xb5, 0x08, 0x8c, 0x0c, 0x67, 0x4b, 0x3e, 0x97,
        0x8f, 0xd6, 0xe4, 0xd7, 0x8e, 0x85, 0x36, 0xd9, 0xdc, 0x64, 0xae, 0xad,
        0x76, 0xac, 0xec, 0x84, 0xa4, 0xd8, 0x54, 0x70, 0xa4, 0x5a, 0x8d, 0x06,
        0x4c, 0xa3, 0x9f, 0xe3, 0x6e, 0xbe, 0xbe, 0x35, 0x77, 0xd6, 0x92, 0xb4,
        0x94, 0x58, 0xc4, 0x77, 0xec, 0xd8, 0x32, 0x68, 0xc9, 0xa5, 0x2a, 0x94,
        0xaa, 0x52, 0xaa, 0x36, 0xb5, 0x8d, 0xe1, 0xb1, 0xbd, 0xf8, 0x9a, 0x72,
        0x10, 0xea, 0x5d, 0xd1, 0xfd, 0x91, 0x31, 0x30, 0x63, 0x33, 0x04, 0x94,
        0x49, 0x44, 0x94, 0x49, 0x44, 0x94, 0x49, 0x44, 0x94, 0x49, 0x53, 0x6d,
        0xa6, 0xce, 0x9b, 0x3a, 0xd7, 0x1a, 0xa6, 0xda, 0x6d, 0x54, 0xd9, 0xd3,
        0x67, 0x4d, 0x9c, 0xcc, 0xe9, 0xb1, 0xa6, 0xce, 0x65, 0xa2, 0x4a, 0x64,
        0xa2, 0x5a, 0x28, 0x94, 0x82, 0x34, 0x4a, 0x32, 0x5d, 0xf2, 0x97, 0x55,
        0xe1, 0x4d, 0x53, 0xd2, 0x9b, 0x04, 0x34, 0xba, 0x23, 0x41, 0xbd, 0x56,
        0x14, 0xea, 0x57, 0xac, 0x25, 0xcb, 0x5a, 0xe3, 0x17, 0x32, 0xe1, 0xe2,
        0x0b, 0xe2, 0x34, 0x4f, 0x41, 0xb1, 0x86, 0xd2, 0xff, 0xa2, 0xde, 0xaf,
        0xb5, 0x9b, 0xfc, 0x0b, 0x0f, 0xa8, 0x7b, 0x0b, 0x01, 0x17, 0xd5, 0x5a,
        0xd9, 0x3d, 0xf2, 0xb5, 0xd5, 0x69, 0x95, 0x0e, 0xb2, 0x6b, 0xaf, 0xdc,
        0xda, 0xa7, 0xf1, 0x0d, 0xd1, 0xf0, 0x9b, 0x6e, 0x18, 0x9e, 0x78, 0xa4,
        0xa6, 0xfb, 0xf6, 0x8e, 0x53, 0x14, 0x57, 0x54, 0xd7, 0x03, 0x32, 0xe3,
        0xed, 0x2c, 0xb9, 0x0f, 0x0d, 0x67, 0x48, 0xd9, 0xa6, 0xec, 0xea, 0x26,
        0xbe, 0xbe, 0xff, 0xf1, 0xb5, 0xdb, 0x0c, 0xc7, 0xde, 0x1c, 0xbd, 0x78,
        0xf9, 0x61, 0xd2,
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
        EXPECT_EQ(pcm.channels, 2u);
        samples.insert(samples.end(), pcm.samples.begin(), pcm.samples.end());
        offset += header.frame_length;
    }

    ASSERT_EQ(samples.size(), 2u * 1024u * 2u);
    EXPECT_NEAR(samples[2048], 0.0806323f, 1e-5f);
    EXPECT_NEAR(samples[2050], 0.0750540f, 1e-5f);
    EXPECT_NEAR(samples[2248], 0.1178071f, 1e-5f);
    EXPECT_NEAR(samples[3048], -0.0240589f, 1e-5f);
    EXPECT_NEAR(samples[3548], 0.1252413f, 1e-5f);
    EXPECT_NEAR(samples[4094], -0.1240385f, 1e-5f);
}

namespace {

using player::audio::opus::Bandwidth;
using player::audio::opus::Mode;
using player::audio::opus::OpusHead;
using player::audio::opus::PacketInfo;

std::vector<uint8_t> makeOpusHead(uint8_t channels, uint16_t pre_skip, uint32_t rate,
                                  int16_t gain, uint8_t family,
                                  const std::vector<uint8_t>& mapping = {},
                                  uint8_t streams = 0, uint8_t coupled = 0) {
    std::vector<uint8_t> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd', 1, channels};
    head.push_back(static_cast<uint8_t>(pre_skip & 0xFF));
    head.push_back(static_cast<uint8_t>(pre_skip >> 8));
    for (int i = 0; i < 4; ++i) {
        head.push_back(static_cast<uint8_t>((rate >> (8 * i)) & 0xFF));
    }
    head.push_back(static_cast<uint8_t>(gain & 0xFF));
    head.push_back(static_cast<uint8_t>((gain >> 8) & 0xFF));
    head.push_back(family);
    if (family != 0) {
        head.push_back(streams);
        head.push_back(coupled);
        head.insert(head.end(), mapping.begin(), mapping.end());
    }
    return head;
}

std::vector<uint8_t> makePacket(uint8_t toc, const std::vector<uint8_t>& payload) {
    std::vector<uint8_t> packet = {toc};
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

}  // namespace

TEST(OpusHead, ParsesFamilyZero) {
    const std::vector<uint8_t> head = makeOpusHead(2, 312, 48000, 0, 0);
    OpusHead parsed;
    std::string error;
    ASSERT_TRUE(player::audio::opus::parseOpusHead(head.data(), head.size(), parsed, &error))
        << error;
    EXPECT_EQ(parsed.version, 1);
    EXPECT_EQ(parsed.channels, 2);
    EXPECT_EQ(parsed.pre_skip, 312);
    EXPECT_EQ(parsed.input_sample_rate, 48000u);
    EXPECT_EQ(parsed.mapping_family, 0);
    EXPECT_EQ(parsed.stream_count, 1);
    EXPECT_EQ(parsed.coupled_count, 1);
    EXPECT_EQ(parsed.channel_mapping[0], 0);
    EXPECT_EQ(parsed.channel_mapping[1], 1);
}

TEST(OpusHead, ParsesFamilyOneMapping) {
    const std::vector<uint8_t> head =
        makeOpusHead(6, 0, 48000, 0, 1, {0, 4, 1, 2, 3, 5}, 4, 2);
    OpusHead parsed;
    std::string error;
    ASSERT_TRUE(player::audio::opus::parseOpusHead(head.data(), head.size(), parsed, &error))
        << error;
    EXPECT_EQ(parsed.channels, 6);
    EXPECT_EQ(parsed.mapping_family, 1);
    EXPECT_EQ(parsed.stream_count, 4);
    EXPECT_EQ(parsed.coupled_count, 2);
    EXPECT_EQ(parsed.channel_mapping[5], 5);
}

TEST(OpusHead, RejectsBadInput) {
    OpusHead parsed;
    std::string error;
    const std::vector<uint8_t> short_head = {'O', 'p', 'u', 's'};
    EXPECT_FALSE(player::audio::opus::parseOpusHead(short_head.data(), short_head.size(), parsed,
                                                    &error));
    std::vector<uint8_t> bad_magic = makeOpusHead(2, 0, 48000, 0, 0);
    bad_magic[0] = 'X';
    EXPECT_FALSE(player::audio::opus::parseOpusHead(bad_magic.data(), bad_magic.size(), parsed,
                                                    &error));
    const std::vector<uint8_t> no_channels = makeOpusHead(0, 0, 48000, 0, 0);
    EXPECT_FALSE(player::audio::opus::parseOpusHead(no_channels.data(), no_channels.size(), parsed,
                                                    &error));
    const std::vector<uint8_t> family2 = makeOpusHead(2, 0, 48000, 0, 2);
    EXPECT_FALSE(player::audio::opus::parseOpusHead(family2.data(), family2.size(), parsed,
                                                    &error));
}

TEST(OpusPacket, ParsesCodeZero) {
    const std::vector<uint8_t> packet = makePacket(0x00, {1, 2, 3, 4});
    PacketInfo info;
    std::string error;
    ASSERT_TRUE(player::audio::opus::parsePacket(packet.data(), packet.size(), info, &error))
        << error;
    EXPECT_EQ(info.mode, Mode::Silk);
    EXPECT_EQ(info.bandwidth, Bandwidth::Narrowband);
    EXPECT_FALSE(info.stereo);
    EXPECT_EQ(info.frame_count, 1);
    EXPECT_EQ(info.frame_samples, 480);
    EXPECT_EQ(info.header_size, 1u);
    EXPECT_EQ(info.frames[0].offset, 1u);
    EXPECT_EQ(info.frames[0].size, 4u);
}

TEST(OpusPacket, ParsesCodeOneCbr) {
    const std::vector<uint8_t> packet = makePacket(0x01, {1, 2, 3, 4});
    PacketInfo info;
    std::string error;
    ASSERT_TRUE(player::audio::opus::parsePacket(packet.data(), packet.size(), info, &error))
        << error;
    EXPECT_EQ(info.frame_count, 2);
    EXPECT_EQ(info.frames[0].offset, 1u);
    EXPECT_EQ(info.frames[0].size, 2u);
    EXPECT_EQ(info.frames[1].offset, 3u);
    EXPECT_EQ(info.frames[1].size, 2u);
}

TEST(OpusPacket, ParsesCodeTwoVbr) {
    const std::vector<uint8_t> packet = makePacket(0x02, {2, 1, 2, 3, 4, 5});
    PacketInfo info;
    std::string error;
    ASSERT_TRUE(player::audio::opus::parsePacket(packet.data(), packet.size(), info, &error))
        << error;
    EXPECT_EQ(info.frame_count, 2);
    EXPECT_EQ(info.header_size, 2u);
    EXPECT_EQ(info.frames[0].offset, 2u);
    EXPECT_EQ(info.frames[0].size, 2u);
    EXPECT_EQ(info.frames[1].offset, 4u);
    EXPECT_EQ(info.frames[1].size, 3u);
}

TEST(OpusPacket, ParsesCodeTwoLongSize) {
    std::vector<uint8_t> payload = {0xFC, 0x00};  // first frame: 252 bytes
    payload.resize(2 + 252 + 5, 0xAA);
    const std::vector<uint8_t> packet = makePacket(0x02, payload);
    PacketInfo info;
    std::string error;
    ASSERT_TRUE(player::audio::opus::parsePacket(packet.data(), packet.size(), info, &error))
        << error;
    EXPECT_EQ(info.header_size, 3u);
    EXPECT_EQ(info.frames[0].size, 252u);
    EXPECT_EQ(info.frames[1].size, 5u);
}

TEST(OpusPacket, ParsesCodeThreeCbr) {
    const std::vector<uint8_t> packet = makePacket(0x03, {0x03, 1, 2, 3, 4, 5, 6});
    PacketInfo info;
    std::string error;
    ASSERT_TRUE(player::audio::opus::parsePacket(packet.data(), packet.size(), info, &error))
        << error;
    EXPECT_EQ(info.frame_count, 3);
    EXPECT_EQ(info.header_size, 2u);
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_EQ(info.frames[i].size, 2u);
        EXPECT_EQ(info.frames[i].offset, 2 + 2 * i);
    }
}

TEST(OpusPacket, ParsesCodeThreeVbr) {
    const std::vector<uint8_t> packet = makePacket(0x03, {0x83, 1, 2, 1, 2, 3, 4, 5, 6});
    PacketInfo info;
    std::string error;
    ASSERT_TRUE(player::audio::opus::parsePacket(packet.data(), packet.size(), info, &error))
        << error;
    EXPECT_EQ(info.frame_count, 3);
    EXPECT_EQ(info.header_size, 4u);
    EXPECT_EQ(info.frames[0].size, 1u);
    EXPECT_EQ(info.frames[1].size, 2u);
    EXPECT_EQ(info.frames[2].size, 3u);
}

TEST(OpusPacket, ParsesCodeThreePadding) {
    // count = 2, padding = 2 bytes at the end of the packet.
    const std::vector<uint8_t> packet = makePacket(0x03, {0x42, 0x02, 1, 2, 3, 4, 0, 0});
    PacketInfo info;
    std::string error;
    ASSERT_TRUE(player::audio::opus::parsePacket(packet.data(), packet.size(), info, &error))
        << error;
    EXPECT_EQ(info.frame_count, 2);
    EXPECT_EQ(info.header_size, 3u);
    EXPECT_EQ(info.frames[0].size, 2u);
    EXPECT_EQ(info.frames[1].size, 2u);
    EXPECT_EQ(info.frames[1].offset + info.frames[1].size, 7u);
}

TEST(OpusPacket, RejectsInvalidPackets) {
    PacketInfo info;
    std::string error;
    const std::vector<uint8_t> empty;
    EXPECT_FALSE(player::audio::opus::parsePacket(empty.data(), 0, info, &error));
    const std::vector<uint8_t> odd = makePacket(0x01, {1, 2, 3});
    EXPECT_FALSE(player::audio::opus::parsePacket(odd.data(), odd.size(), info, &error));
    const std::vector<uint8_t> zero_count = makePacket(0x03, {0x00, 1, 2});
    EXPECT_FALSE(player::audio::opus::parsePacket(zero_count.data(), zero_count.size(), info,
                                                  &error));
    // 60 ms frames: at most 2 fit into the 120 ms packet limit.
    const std::vector<uint8_t> too_long = makePacket(0x1B, {0x03, 1, 2, 3, 4, 5, 6});
    EXPECT_FALSE(player::audio::opus::parsePacket(too_long.data(), too_long.size(), info, &error));
}

TEST(OpusPacket, ModeBandwidthAndFrameSizes) {
    PacketInfo info;
    std::string error;
    const std::vector<uint8_t> payload = {1, 2, 3, 4};
    const struct {
        uint8_t toc;
        Mode mode;
        Bandwidth bandwidth;
        uint16_t samples;
        bool stereo;
    } cases[] = {
        {0x00, Mode::Silk, Bandwidth::Narrowband, 480, false},
        {0x18, Mode::Silk, Bandwidth::Narrowband, 2880, false},
        {0x60, Mode::Hybrid, Bandwidth::SuperWideband, 480, false},
        {0x78, Mode::Hybrid, Bandwidth::Fullband, 960, false},
        {0x80, Mode::Celt, Bandwidth::Narrowband, 120, false},
        {0x84, Mode::Celt, Bandwidth::Narrowband, 120, true},
        {0xA0, Mode::Celt, Bandwidth::Wideband, 120, false},
        {0xC0, Mode::Celt, Bandwidth::SuperWideband, 120, false},
        {0xF8, Mode::Celt, Bandwidth::Fullband, 960, false},
    };
    for (const auto& test : cases) {
        const std::vector<uint8_t> packet = makePacket(test.toc, payload);
        ASSERT_TRUE(player::audio::opus::parsePacket(packet.data(), packet.size(), info, &error))
            << error;
        EXPECT_EQ(info.mode, test.mode);
        EXPECT_EQ(info.bandwidth, test.bandwidth);
        EXPECT_EQ(info.frame_samples, test.samples);
        EXPECT_EQ(info.stereo, test.stereo) << "toc=" << static_cast<int>(test.toc);
    }
}

TEST(OpusRangeDecoder, DecodesReferenceVector) {
    // 145 bytes produced by the RFC 6716 reference range encoder for a known
    // symbol sequence (uniform, bit_logp, icdf, uint and raw bits).
    static const uint8_t kRangeVector[] = {
        0x18, 0xad, 0xaa, 0x2e, 0xb1, 0x3e, 0xe7, 0x92, 0x07, 0xd8, 0x5a, 0x56,
        0x20, 0x03, 0x53, 0x71, 0x8f, 0x86, 0x95, 0x17, 0x1b, 0xcf, 0x0c, 0x5c,
        0xaf, 0x06, 0xa7, 0xab, 0xd7, 0x1d, 0x62, 0x41, 0x3e, 0x8e, 0x62, 0x1e,
        0x3f, 0x5b, 0x72, 0x9e, 0xd3, 0x2c, 0x77, 0x4b, 0x20, 0xc4, 0x89, 0xb8,
        0x94, 0xd9, 0x89, 0xd6, 0xfa, 0xec, 0x58, 0x12, 0x7b, 0x45, 0xe9, 0x21,
        0x9b, 0xdd, 0x41, 0xc7, 0xa7, 0xf6, 0xe3, 0xf6, 0x52, 0x90, 0xf4, 0xf5,
        0x69, 0xfe, 0xe3, 0xf1, 0xe8, 0x2d, 0xeb, 0x9f, 0xe8, 0xc9, 0x47, 0xa9,
        0xdb, 0x0d, 0xbe, 0xd4, 0xdb, 0x5b, 0xb1, 0x59, 0x45, 0x00, 0x57, 0x73,
        0xc6, 0xb3, 0xcb, 0x3e, 0xab, 0xb5, 0x33, 0x84, 0x9d, 0xc8, 0x69, 0x96,
        0x35, 0xbe, 0x7e, 0xde, 0xaa, 0x1c, 0x03, 0xa0, 0x18, 0x55, 0x90, 0x06,
        0x80, 0xa3, 0x6a, 0x9e, 0x9a, 0xdc, 0x66, 0x7f, 0xf0, 0x14, 0xa4, 0x1f,
        0x81, 0x17, 0x75, 0xa0, 0x46, 0xbd, 0xef, 0x1d, 0x11, 0x45, 0x78, 0xb8,
        0xdc,
    };
    static const uint8_t kIcdf[6] = {200, 150, 90, 40, 10, 0};

    player::audio::opus::RangeDecoder decoder(kRangeVector, sizeof(kRangeVector));
    uint32_t state = 12345;
    const auto random = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };

    for (int i = 0; i < 60; ++i) {
        const uint32_t total = 2 + random() % 999;
        const uint32_t expected = random() % total;
        const uint32_t value = decoder.decode(total);
        decoder.update(value, value + 1, total);
        EXPECT_EQ(value, expected) << "uniform " << i;
    }
    for (int i = 0; i < 40; ++i) {
        const unsigned logp = 1 + random() % 15;
        const bool expected = (random() & 1) != 0;
        EXPECT_EQ(decoder.decodeBitLogp(logp), expected) << "bit_logp " << i;
    }
    for (int i = 0; i < 40; ++i) {
        const int expected = static_cast<int>(random() % 6);
        EXPECT_EQ(decoder.decodeIcdf(kIcdf, 8), expected) << "icdf " << i;
    }
    for (int i = 0; i < 20; ++i) {
        const uint32_t expected = random() % 100001;
        EXPECT_EQ(decoder.decodeUint(100001), expected) << "uint " << i;
    }
    for (int i = 0; i < 8; ++i) {
        const uint32_t expected = random() & 0xFF;
        EXPECT_EQ(decoder.decodeBits(8), expected) << "bits " << i;
    }
    EXPECT_FALSE(decoder.error());
}

TEST(OpusDecoder, ConfiguresAndRejectsUnimplementedLayers) {
    const std::vector<uint8_t> head = makeOpusHead(2, 312, 48000, 0, 0);
    player::audio::opus::Decoder decoder;
    std::string error;
    ASSERT_TRUE(decoder.configure(head.data(), head.size(), error)) << error;
    EXPECT_TRUE(decoder.configured());
    EXPECT_EQ(decoder.channels(), 2u);
    EXPECT_EQ(decoder.sampleRate(), 48000u);

    const std::vector<uint8_t> packet = makePacket(0x00, {1, 2, 3, 4});
    player::audio::PcmBuffer pcm;
    EXPECT_EQ(decoder.decode(packet.data(), packet.size(), pcm, error),
              player::audio::opus::Status::Unsupported);

    const std::vector<uint8_t> bad = makePacket(0x01, {1, 2, 3});
    EXPECT_EQ(decoder.decode(bad.data(), bad.size(), pcm, error),
              player::audio::opus::Status::Invalid);
}

TEST(CeltPvq, CountsCodewords) {
    EXPECT_EQ(player::audio::opus::pvqCount(2, 1), 4u);
    EXPECT_EQ(player::audio::opus::pvqCount(2, 2), 8u);
    EXPECT_EQ(player::audio::opus::pvqCount(3, 1), 6u);
    EXPECT_EQ(player::audio::opus::pvqCount(2, 16), 64u);
}

TEST(CeltPvq, DecodesReferenceVectors) {
    // Packets produced by the reference encoder (encode_pulses) for known
    // pulse vectors; only the non-zero entries are listed.
    struct Case {
        int n;
        int k;
        std::vector<uint8_t> packet;
        std::vector<std::pair<int, int>> nonzero;
    };
    const std::vector<Case> cases = {
        {2, 1, {0x00}, {{0, 1}}},
        {2, 2, {0x20}, {{0, 1}, {1, 1}}},
        {2, 16, {0x00}, {{0, 16}}},
        {3, 1, {0xe0}, {{0, -1}}},
        {4, 3, {0x00}, {{0, 3}}},
        {5, 5, {0xaf, 0x80, 0x01}, {{0, -4}, {1, -1}}},
        {8, 2, {0x02}, {{0, 1}, {1, 1}}},
        {8, 8, {0xb7, 0x02, 0x36}, {{0, -3}, {5, 5}}},
        {12, 10, {0x00, 0x00, 0x00, 0x02}, {{0, 9}, {2, 1}}},
        {16, 4, {0xe2, 0x80, 0x00, 0x81}, {{0, -4}}},
        {24, 6, {0xed, 0x00, 0x4a, 0x5e}, {{0, -1}, {4, 1}, {7, 2}, {11, 1}, {13, 1}}},
        {32, 3, {0xf3, 0x00, 0xff}, {{0, -3}}},
        {48, 4, {0x00, 0x22, 0x86}, {{0, 1}, {2, 3}}},
        {64, 2, {0x00, 0x01}, {{0, 1}, {1, 1}}},
        {96, 3, {0x00, 0x00, 0x00}, {{0, 3}}},
        {100, 1, {0xff}, {{0, -1}}},
        {120, 2, {0x00, 0x01}, {{0, 1}, {1, 1}}},
        {176, 1, {0xff, 0x01}, {{0, -1}}},
        {176, 2, {0xfe, 0x00, 0x00, 0xa2}, {{0, -1}, {1, 1}}},
        {4, 1, {0x00}, {{0, 1}}},
    };

    for (const Case& test : cases) {
        player::audio::opus::RangeDecoder decoder(test.packet.data(), test.packet.size());
        std::vector<int> pulses(static_cast<size_t>(test.n), 0);
        const float energy =
            player::audio::opus::decodePulses(decoder, test.n, test.k, pulses.data());

        int pulse_sum = 0;
        for (int value : pulses) {
            pulse_sum += value < 0 ? -value : value;
        }
        EXPECT_EQ(pulse_sum, test.k) << "n=" << test.n << " k=" << test.k;

        float expected_energy = 0.0f;
        for (const auto& entry : test.nonzero) {
            EXPECT_EQ(pulses[static_cast<size_t>(entry.first)], entry.second)
                << "n=" << test.n << " k=" << test.k << " pos=" << entry.first;
            expected_energy += static_cast<float>(entry.second) * static_cast<float>(entry.second);
        }
        EXPECT_FLOAT_EQ(energy, expected_energy);
        EXPECT_FALSE(decoder.error());
    }
}

TEST(CeltPvq, IndexRoundTripIsInjective) {
    // Every index below V(n, k) must produce a distinct vector with k pulses.
    for (const auto& test : {std::pair<int, int>{4, 3}, std::pair<int, int>{8, 4}}) {
        const int n = test.first;
        const int k = test.second;
        const uint32_t count = player::audio::opus::pvqCount(n, k);
        std::vector<int> pulses(static_cast<size_t>(n), 0);
        std::vector<std::vector<int>> seen;
        for (uint32_t index = 0; index < count; ++index) {
            player::audio::opus::indexToPulses(n, k, index, pulses.data());
            int sum = 0;
            for (int value : pulses) {
                sum += value < 0 ? -value : value;
            }
            EXPECT_EQ(sum, k);
            seen.push_back(pulses);
        }
        std::sort(seen.begin(), seen.end());
        EXPECT_EQ(std::unique(seen.begin(), seen.end()), seen.end());
    }
}

TEST(CeltTables, ValuesMatchReference) {
    EXPECT_EQ(player::audio::opus::kCeltEBands[0], 0);
    EXPECT_EQ(player::audio::opus::kCeltEBands[21], 100);
    EXPECT_EQ(player::audio::opus::kCeltEBands[9], 10);
    EXPECT_EQ(player::audio::opus::kCeltLogN[8], 8);
    EXPECT_EQ(player::audio::opus::kCeltLogN[20], 36);
    EXPECT_NEAR(player::audio::opus::kCeltWindow[0], 6.7286966e-05f, 1e-10f);
    EXPECT_NEAR(player::audio::opus::kCeltWindow[119], 1.0f, 1e-6f);
    EXPECT_EQ(player::audio::opus::kCeltBandAllocation[0], 0);
}

TEST(CeltEnergy, DecodesReferenceEnergies) {
    // Produced by the reference encoder/decoder pair: coarse energies with the
    // Laplace model, fine energy and final bits, decoded back for comparison.
    static const uint8_t kPacket[] = {
        0x7c, 0x18, 0x2f, 0x74, 0xcb, 0x53, 0x9b, 0xe0, 0x00, 0xbb, 0x26, 0xa4,
        0x29, 0xc8, 0xd3, 0x28, 0xa9, 0x53, 0x54, 0x75, 0xd0, 0x63, 0x20, 0x09,
        0xc1, 0x3d, 0xad, 0x55, 0xd0, 0xb1, 0x6e, 0x32, 0x08, 0x38, 0xf7, 0xb3,
        0x4a, 0xad, 0x21, 0x5a, 0xd7, 0x2f, 0xac, 0xce, 0x6a, 0xe5, 0xba, 0x48,
        0xe0, 0xdf, 0x80, 0xf9, 0xff, 0x28, 0xbe, 0xa3, 0x79, 0xe9, 0xc4, 0x00,
        0x3e, 0xaa, 0xc2, 0x43, 0xaa, 0x62, 0xf4, 0x8b, 0x9d, 0x5b, 0x68, 0xfe,
        0xa8, 0x25, 0xbb, 0x61, 0x2a, 0xed, 0xd1, 0x2a, 0x73, 0xaa, 0x42, 0xe8,
        0xdd, 0x45, 0x4f, 0x4e,
    };
    static const float kInit[42] = {
        4.71999979f, 8.72999954f, -12.2399998f, 7.80000019f, -15.3299999f, -14.6400003f,
        7.96999979f, -2.29999995f, 4.01999998f, -3.63000011f, 10.0200005f, 16.6000004f,
        -10.5900002f, -16.4500008f, 14.79f, 18.6299992f, -10.4399996f, -18.6800003f,
        -1.97000003f, 0.460000008f, -7.57999992f, 4.57999992f, -0.519999981f, -1.96000004f,
        -15.9499998f, 7.11000013f, -8.82999992f, -15.4799995f, 6.69000006f, -10.1199999f,
        5.17000008f, -0.0199999996f, 16.7800007f, 7.96999979f, -8.34000015f, 17.7600002f,
        -17.0599995f, -19.9300003f, 17.3899994f, 13.6700001f, -5.67000008f, 16.4400005f,
    };
    static const int kFineQuant[21] = {7, 3, 3, 4, 3, 4, 3, 6, 8, 6, 5,
                                       1, 6, 2, 7, 5, 5, 6, 3, 1, 5};
    static const int kFinePriority[21] = {1, 1, 1, 0, 1, 1, 0, 0, 0, 1, 0,
                                          0, 1, 0, 1, 1, 0, 1, 1, 1, 1};
    static const float kExpected[42] = {
        2.17195296f, 12.1871042f, -6.0246582f, 5.27094746f, 0.637817383f, 7.21472168f,
        14.0293751f, 11.6813231f, 22.3678284f, 17.0820847f, 26.4159241f, 39.4192123f,
        18.5869141f, 24.2967529f, 39.7706871f, 42.4484329f, 21.6152344f, 32.916626f,
        30.4665985f, 26.1835365f, 20.0053749f, -6.28992176f, -0.225981236f, -8.37970161f,
        -1.14160156f, 12.3391504f, 0.111210823f, -2.94714355f, 15.2391701f, -7.6907959f,
        -3.58959484f, -13.8945608f, -4.074296f, -11.9722118f, -15.5955906f, 0.0442047119f,
        -25.4388428f, -27.5257568f, -6.84085941f, -0.692255974f, -25.781744f, -0.138271332f,
    };

    float energy[42];
    std::copy(std::begin(kInit), std::end(kInit), energy);

    player::audio::opus::RangeDecoder decoder(kPacket, sizeof(kPacket));
    player::audio::opus::decodeCoarseEnergy(decoder, 0, 21, energy, /*intra=*/false, 2, 2);
    player::audio::opus::decodeFineEnergy(decoder, 0, 21, energy, kFineQuant, 2);
    player::audio::opus::decodeFinalEnergy(decoder, 0, 21, energy, kFineQuant, kFinePriority, 40, 2);

    for (int i = 0; i < 42; ++i) {
        EXPECT_NEAR(energy[i], kExpected[i], 1e-4f) << "band " << i;
    }
}
