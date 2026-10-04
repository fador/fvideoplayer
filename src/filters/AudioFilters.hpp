#pragma once

#include "filters/Filter.hpp"

#include <array>
#include <vector>

namespace player::filters {

// Applies a linear gain to the chunk, clamping to [-1, 1].
class VolumeFilter : public IAudioFilter {
public:
    explicit VolumeFilter(float gain) : gain_(gain) {}

    const char* name() const override { return "volume"; }
    bool configure(const AudioFormat& input, AudioFormat& output, std::string& error) override;
    bool process(audio::PcmBuffer& buffer, std::string& error) override;

private:
    float gain_ = 1.0f;
};

// Linear-interpolation sample rate converter with one input sample of state so
// chunk boundaries stay continuous.
class ResampleFilter : public IAudioFilter {
public:
    explicit ResampleFilter(uint32_t sample_rate) : target_rate_(sample_rate) {}

    const char* name() const override { return "resample"; }
    bool configure(const AudioFormat& input, AudioFormat& output, std::string& error) override;
    bool process(audio::PcmBuffer& buffer, std::string& error) override;
    void reset() override;

private:
    uint32_t target_rate_ = 0;
    bool have_prev_ = false;
    double position_ = 0.0;
    std::vector<float> prev_;
};

// Remixes between mono and stereo.
class ChannelsFilter : public IAudioFilter {
public:
    explicit ChannelsFilter(uint32_t channels) : target_channels_(channels) {}

    const char* name() const override { return "channels"; }
    bool configure(const AudioFormat& input, AudioFormat& output, std::string& error) override;
    bool process(audio::PcmBuffer& buffer, std::string& error) override;

private:
    uint32_t target_channels_ = 0;
};

// Three-band equalizer: low shelf, peaking mid and high shelf, all in dB.
class EqualizerFilter : public IAudioFilter {
public:
    EqualizerFilter(double bass_db, double mid_db, double treble_db)
        : bass_db_(bass_db), mid_db_(mid_db), treble_db_(treble_db) {}

    const char* name() const override { return "equalizer"; }
    bool configure(const AudioFormat& input, AudioFormat& output, std::string& error) override;
    bool process(audio::PcmBuffer& buffer, std::string& error) override;
    void reset() override;

private:
    struct Biquad {
        double b0 = 1.0;
        double b1 = 0.0;
        double b2 = 0.0;
        double a1 = 0.0;
        double a2 = 0.0;
        double x1 = 0.0;
        double x2 = 0.0;
        double y1 = 0.0;
        double y2 = 0.0;

        float process(float sample);
    };

    double bass_db_ = 0.0;
    double mid_db_ = 0.0;
    double treble_db_ = 0.0;
    bool bypass_ = true;
    std::vector<std::array<Biquad, 3>> channels_;
};

}  // namespace player::filters
