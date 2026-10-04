#include "decode/Frame.hpp"
#include "filters/Filter.hpp"
#include "filters/FilterSpec.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using player::decode::ChromaFormat;
using player::decode::Frame;
using player::filters::AudioFilterChain;
using player::filters::VideoFilterChain;

Frame makeFrame(int width, int height, ChromaFormat format = ChromaFormat::C420) {
    Frame frame;
    EXPECT_TRUE(frame.alloc(width, height, format, 8));
    return frame;
}

uint8_t luma(const Frame& frame, int x, int y) {
    return frame.row(0, y)[x];
}

uint8_t chroma(const Frame& frame, int plane, int x, int y) {
    return frame.row(plane, y)[x];
}

void fillChroma(Frame& frame, uint8_t value) {
    for (int plane = 1; plane < 3; ++plane) {
        for (int y = 0; y < frame.planeHeight(plane); ++y) {
            uint8_t* row = frame.row(plane, y);
            for (int x = 0; x < frame.planeWidth(plane); ++x) {
                row[x] = value;
            }
        }
    }
}

std::shared_ptr<VideoFilterChain> videoChain(const std::string& spec, std::string& error) {
    return player::filters::parseVideoFilterSpec(spec, error);
}

std::shared_ptr<AudioFilterChain> audioChain(const std::string& spec, std::string& error) {
    return player::filters::parseAudioFilterSpec(spec, error);
}

player::audio::PcmBuffer makeAudio(uint32_t channels, uint32_t sample_rate, size_t frames) {
    player::audio::PcmBuffer buffer;
    buffer.channels = channels;
    buffer.sample_rate = sample_rate;
    buffer.samples.assign(frames * channels, 0.0f);
    return buffer;
}

TEST(FilterSpec, ParsesVideoGraph) {
    std::string error;
    const auto chain = videoChain("scale=640:360,flip=h,eq=brightness=0.5:contrast=1.5", error);
    ASSERT_NE(chain, nullptr) << error;
    EXPECT_EQ(chain->size(), 3u);
    const std::vector<std::string> names = chain->names();
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "scale");
    EXPECT_EQ(names[1], "flip");
    EXPECT_EQ(names[2], "eq");
}

TEST(FilterSpec, ParsesAudioGraph) {
    std::string error;
    const auto chain = audioChain("volume=1.5,resample=48000,equalizer=bass=6:treble=-3", error);
    ASSERT_NE(chain, nullptr) << error;
    EXPECT_EQ(chain->size(), 3u);
    const std::vector<std::string> names = chain->names();
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[0], "volume");
    EXPECT_EQ(names[1], "resample");
    EXPECT_EQ(names[2], "equalizer");
}

TEST(FilterSpec, EmptySpecIsEmptyChain) {
    std::string error;
    const auto video = videoChain("", error);
    ASSERT_NE(video, nullptr);
    EXPECT_TRUE(video->empty());
    const auto audio = audioChain("", error);
    ASSERT_NE(audio, nullptr);
    EXPECT_TRUE(audio->empty());
}

TEST(FilterSpec, RejectsUnknownAndMalformed) {
    std::string error;
    EXPECT_EQ(videoChain("nosuch", error), nullptr);
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_EQ(videoChain("scale=abc:360", error), nullptr);
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_EQ(videoChain("flip=x", error), nullptr);
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_EQ(audioChain("resample=0", error), nullptr);
    EXPECT_FALSE(error.empty());

    error.clear();
    EXPECT_EQ(audioChain("channels=5", error), nullptr);
    EXPECT_FALSE(error.empty());
}

TEST(VideoFilters, ScaleDoublesSize) {
    std::string error;
    const auto chain = videoChain("scale=4:4", error);
    ASSERT_NE(chain, nullptr) << error;
    Frame frame = makeFrame(2, 2);
    frame.row(0, 0)[0] = 0;
    frame.row(0, 0)[1] = 100;
    frame.row(0, 1)[0] = 200;
    frame.row(0, 1)[1] = 255;

    ASSERT_TRUE(chain->process(frame, error)) << error;
    EXPECT_EQ(frame.width_, 4);
    EXPECT_EQ(frame.height_, 4);
    EXPECT_EQ(luma(frame, 0, 0), 0);
    EXPECT_EQ(luma(frame, 3, 0), 100);
    EXPECT_EQ(luma(frame, 1, 0), 25);
    EXPECT_EQ(luma(frame, 2, 0), 75);
    EXPECT_EQ(luma(frame, 0, 1), 50);
    EXPECT_EQ(luma(frame, 0, 3), 200);
    EXPECT_EQ(luma(frame, 3, 3), 255);
}

TEST(VideoFilters, ScaleDerivesMissingDimension) {
    std::string error;
    auto filter = player::filters::createVideoFilter("scale=640:-2", error);
    ASSERT_NE(filter, nullptr) << error;
    player::filters::VideoFormat input;
    input.width = 1920;
    input.height = 1080;
    player::filters::VideoFormat output;
    ASSERT_TRUE(filter->configure(input, output, error)) << error;
    EXPECT_EQ(output.width, 640);
    EXPECT_EQ(output.height, 360);

    auto odd = player::filters::createVideoFilter("scale=100:-2", error);
    ASSERT_NE(odd, nullptr) << error;
    input.width = 640;
    input.height = 360;
    ASSERT_TRUE(odd->configure(input, output, error)) << error;
    EXPECT_EQ(output.width, 100);
    EXPECT_EQ(output.height, 56);
}

TEST(VideoFilters, CropCopiesRectangle) {
    std::string error;
    const auto chain = videoChain("crop=2:2:1:1", error);
    ASSERT_NE(chain, nullptr) << error;
    Frame frame = makeFrame(4, 4, ChromaFormat::C444);
    for (int y = 0; y < 4; ++y) {
        for (int x = 0; x < 4; ++x) {
            frame.row(0, y)[x] = static_cast<uint8_t>(x + y * 10);
        }
    }
    ASSERT_TRUE(chain->process(frame, error)) << error;
    EXPECT_EQ(frame.width_, 2);
    EXPECT_EQ(frame.height_, 2);
    EXPECT_EQ(luma(frame, 0, 0), 11);
    EXPECT_EQ(luma(frame, 1, 1), 22);
}

TEST(VideoFilters, CropCenterAndAlignment) {
    std::string error;
    auto centered = player::filters::createVideoFilter("crop=2:2", error);
    ASSERT_NE(centered, nullptr) << error;
    player::filters::VideoFormat input;
    input.width = 4;
    input.height = 4;
    player::filters::VideoFormat output;
    ASSERT_TRUE(centered->configure(input, output, error)) << error;
    EXPECT_EQ(output.width, 2);
    EXPECT_EQ(output.height, 2);

    const auto odd = videoChain("crop=2:2:1:0", error);
    ASSERT_NE(odd, nullptr) << error;
    Frame frame = makeFrame(4, 4);
    EXPECT_FALSE(odd->process(frame, error));
    EXPECT_NE(error.find("even"), std::string::npos) << error;
}

TEST(VideoFilters, Flip) {
    std::string error;
    auto horizontal = videoChain("flip=h", error);
    ASSERT_NE(horizontal, nullptr) << error;
    Frame frame = makeFrame(3, 1);
    frame.row(0, 0)[0] = 1;
    frame.row(0, 0)[1] = 2;
    frame.row(0, 0)[2] = 3;
    ASSERT_TRUE(horizontal->process(frame, error)) << error;
    EXPECT_EQ(luma(frame, 0, 0), 3);
    EXPECT_EQ(luma(frame, 1, 0), 2);
    EXPECT_EQ(luma(frame, 2, 0), 1);

    error.clear();
    auto vertical = videoChain("flip=v", error);
    ASSERT_NE(vertical, nullptr) << error;
    Frame rows = makeFrame(1, 2);
    rows.row(0, 0)[0] = 10;
    rows.row(0, 1)[0] = 20;
    ASSERT_TRUE(vertical->process(rows, error)) << error;
    EXPECT_EQ(luma(rows, 0, 0), 20);
    EXPECT_EQ(luma(rows, 0, 1), 10);
}

TEST(VideoFilters, GrayscaleNeutralizesChroma) {
    std::string error;
    const auto chain = videoChain("grayscale", error);
    ASSERT_NE(chain, nullptr) << error;
    Frame frame = makeFrame(2, 2);
    fillChroma(frame, 42);
    ASSERT_TRUE(chain->process(frame, error)) << error;
    EXPECT_EQ(chroma(frame, 1, 0, 0), 128);
    EXPECT_EQ(chroma(frame, 2, 0, 0), 128);
}

TEST(VideoFilters, EqBrightnessContrastSaturation) {
    std::string error;
    const auto flat = videoChain("eq=contrast=0:saturation=0", error);
    ASSERT_NE(flat, nullptr) << error;
    Frame frame = makeFrame(2, 1);
    frame.row(0, 0)[0] = 100;
    frame.row(0, 0)[1] = 200;
    fillChroma(frame, 40);
    ASSERT_TRUE(flat->process(frame, error)) << error;
    // contrast 0 pins luma to mid grey, saturation 0 pins chroma to neutral.
    EXPECT_EQ(luma(frame, 0, 0), 128);
    EXPECT_EQ(luma(frame, 1, 0), 128);
    EXPECT_EQ(chroma(frame, 1, 0, 0), 128);
    EXPECT_EQ(chroma(frame, 2, 0, 0), 128);

    error.clear();
    const auto bright = videoChain("eq=brightness=0.5", error);
    ASSERT_NE(bright, nullptr) << error;
    Frame lifted = makeFrame(1, 1);
    lifted.row(0, 0)[0] = 100;
    ASSERT_TRUE(bright->process(lifted, error)) << error;
    // 100 + 0.5*255 rounds to 228 on 8-bit samples.
    EXPECT_EQ(luma(lifted, 0, 0), 228);
}

TEST(VideoFilters, ChainOrderIsLeftToRight) {
    std::string error;
    const auto chain = videoChain("scale=4:4,flip=h", error);
    ASSERT_NE(chain, nullptr) << error;
    Frame frame = makeFrame(2, 2);
    frame.row(0, 0)[0] = 0;
    frame.row(0, 0)[1] = 100;
    ASSERT_TRUE(chain->process(frame, error)) << error;
    ASSERT_EQ(frame.width_, 4);
    EXPECT_EQ(luma(frame, 0, 0), 100);
    EXPECT_EQ(luma(frame, 1, 0), 75);
    EXPECT_EQ(luma(frame, 2, 0), 25);
    EXPECT_EQ(luma(frame, 3, 0), 0);
}

TEST(AudioFilters, VolumeAppliesGainAndClamps) {
    std::string error;
    const auto chain = audioChain("volume=0.5", error);
    ASSERT_NE(chain, nullptr) << error;
    player::audio::PcmBuffer buffer = makeAudio(1, 48000, 3);
    buffer.samples = {1.0f, -0.5f, 2.0f};
    ASSERT_TRUE(chain->process(buffer, error)) << error;
    EXPECT_FLOAT_EQ(buffer.samples[0], 0.5f);
    EXPECT_FLOAT_EQ(buffer.samples[1], -0.25f);
    EXPECT_FLOAT_EQ(buffer.samples[2], 1.0f);

    error.clear();
    const auto decibels = player::filters::createAudioFilter("volume=-6dB", error);
    ASSERT_NE(decibels, nullptr) << error;
    player::filters::AudioFormat input;
    input.sample_rate = 48000;
    input.channels = 1;
    player::filters::AudioFormat output;
    ASSERT_TRUE(decibels->configure(input, output, error)) << error;
    player::audio::PcmBuffer one = makeAudio(1, 48000, 1);
    one.samples = {1.0f};
    ASSERT_TRUE(decibels->process(one, error)) << error;
    EXPECT_NEAR(one.samples[0], 0.501187f, 1e-4f);
}

TEST(AudioFilters, ResampleChangesRateAndLength) {
    std::string error;
    const auto chain = audioChain("resample=24000", error);
    ASSERT_NE(chain, nullptr) << error;
    player::audio::PcmBuffer buffer = makeAudio(1, 48000, 480);
    for (float& sample : buffer.samples) {
        sample = 0.75f;
    }
    ASSERT_TRUE(chain->process(buffer, error)) << error;
    EXPECT_EQ(buffer.sample_rate, 24000u);
    EXPECT_EQ(buffer.channels, 1u);
    EXPECT_EQ(buffer.frames(), 240u);
    for (const float sample : buffer.samples) {
        EXPECT_NEAR(sample, 0.75f, 1e-5f);
    }
}

TEST(AudioFilters, ResampleIsContinuousAcrossChunks) {
    std::string error;
    const auto chain = audioChain("resample=48000", error);
    ASSERT_NE(chain, nullptr) << error;
    player::audio::PcmBuffer first = makeAudio(1, 24000, 2);
    first.samples = {0.0f, 1.0f};
    ASSERT_TRUE(chain->process(first, error)) << error;
    player::audio::PcmBuffer second = makeAudio(1, 24000, 2);
    second.samples = {1.0f, 1.0f};
    ASSERT_TRUE(chain->process(second, error)) << error;
    // 2x upsampling: the first chunk emits the start sample and the midpoint
    // toward the next chunk, which state carries over.
    ASSERT_EQ(first.samples.size(), 2u);
    EXPECT_NEAR(first.samples[0], 0.0f, 1e-5f);
    EXPECT_NEAR(first.samples[1], 0.5f, 1e-5f);
    for (const float sample : second.samples) {
        EXPECT_NEAR(sample, 1.0f, 1e-5f);
    }
}

TEST(AudioFilters, ChannelsMonoAndStereo) {
    std::string error;
    const auto to_mono = audioChain("channels=1", error);
    ASSERT_NE(to_mono, nullptr) << error;
    player::audio::PcmBuffer stereo = makeAudio(2, 48000, 2);
    stereo.samples = {1.0f, 0.0f, 0.5f, 0.5f};
    ASSERT_TRUE(to_mono->process(stereo, error)) << error;
    EXPECT_EQ(stereo.channels, 1u);
    ASSERT_EQ(stereo.samples.size(), 2u);
    EXPECT_FLOAT_EQ(stereo.samples[0], 0.5f);
    EXPECT_FLOAT_EQ(stereo.samples[1], 0.5f);

    error.clear();
    const auto to_stereo = audioChain("channels=2", error);
    ASSERT_NE(to_stereo, nullptr) << error;
    player::audio::PcmBuffer mono = makeAudio(1, 48000, 2);
    mono.samples = {1.0f, -0.5f};
    ASSERT_TRUE(to_stereo->process(mono, error)) << error;
    EXPECT_EQ(mono.channels, 2u);
    ASSERT_EQ(mono.samples.size(), 4u);
    EXPECT_FLOAT_EQ(mono.samples[0], 1.0f);
    EXPECT_FLOAT_EQ(mono.samples[1], 1.0f);
    EXPECT_FLOAT_EQ(mono.samples[2], -0.5f);
    EXPECT_FLOAT_EQ(mono.samples[3], -0.5f);
}

TEST(AudioFilters, EqualizerReachesDcGain) {
    std::string error;
    const auto chain = audioChain("equalizer=bass=6", error);
    ASSERT_NE(chain, nullptr) << error;
    player::audio::PcmBuffer buffer = makeAudio(1, 48000, 48000);
    for (float& sample : buffer.samples) {
        sample = 0.1f;
    }
    ASSERT_TRUE(chain->process(buffer, error)) << error;
    const double expected = 0.1 * std::pow(10.0, 6.0 / 20.0);
    EXPECT_NEAR(buffer.samples.back(), expected, 0.002);
}

TEST(AudioFilters, EqualizerZeroGainIsTransparent) {
    std::string error;
    const auto chain = audioChain("equalizer=bass=0:mid=0:treble=0", error);
    ASSERT_NE(chain, nullptr) << error;
    player::audio::PcmBuffer buffer = makeAudio(1, 48000, 8);
    for (size_t i = 0; i < buffer.samples.size(); ++i) {
        buffer.samples[i] = static_cast<float>(i) * 0.05f - 0.2f;
    }
    const std::vector<float> original = buffer.samples;
    ASSERT_TRUE(chain->process(buffer, error)) << error;
    EXPECT_EQ(buffer.samples, original);
}

}  // namespace
