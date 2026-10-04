#include "filters/AudioFilters.hpp"

#include "audio/PcmGain.hpp"

#include <algorithm>
#include <cmath>

namespace player::filters {
namespace {

constexpr double kPi = 3.14159265358979323846;

struct Coefficients {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

double clampFrequency(double rate, double frequency) {
    const double nyquist = rate * 0.45;
    return std::max(20.0, std::min(frequency, nyquist));
}

Coefficients lowShelf(double rate, double frequency, double gain_db) {
    const double A = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * kPi * frequency / rate;
    const double cos_w0 = std::cos(w0);
    const double alpha = std::sin(w0) / 2.0 * std::sqrt(2.0);
    const double two_sqrt_a_alpha = 2.0 * std::sqrt(A) * alpha;
    Coefficients c;
    c.b0 = A * ((A + 1.0) - (A - 1.0) * cos_w0 + two_sqrt_a_alpha);
    c.b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cos_w0);
    c.b2 = A * ((A + 1.0) - (A - 1.0) * cos_w0 - two_sqrt_a_alpha);
    const double a0 = (A + 1.0) + (A - 1.0) * cos_w0 + two_sqrt_a_alpha;
    const double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cos_w0);
    const double a2 = (A + 1.0) + (A - 1.0) * cos_w0 - two_sqrt_a_alpha;
    c.b0 /= a0;
    c.b1 /= a0;
    c.b2 /= a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

Coefficients highShelf(double rate, double frequency, double gain_db) {
    const double A = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * kPi * frequency / rate;
    const double cos_w0 = std::cos(w0);
    const double alpha = std::sin(w0) / 2.0 * std::sqrt(2.0);
    const double two_sqrt_a_alpha = 2.0 * std::sqrt(A) * alpha;
    Coefficients c;
    c.b0 = A * ((A + 1.0) + (A - 1.0) * cos_w0 + two_sqrt_a_alpha);
    c.b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cos_w0);
    c.b2 = A * ((A + 1.0) + (A - 1.0) * cos_w0 - two_sqrt_a_alpha);
    const double a0 = (A + 1.0) - (A - 1.0) * cos_w0 + two_sqrt_a_alpha;
    const double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cos_w0);
    const double a2 = (A + 1.0) - (A - 1.0) * cos_w0 - two_sqrt_a_alpha;
    c.b0 /= a0;
    c.b1 /= a0;
    c.b2 /= a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

Coefficients peaking(double rate, double frequency, double q, double gain_db) {
    const double A = std::pow(10.0, gain_db / 40.0);
    const double w0 = 2.0 * kPi * frequency / rate;
    const double cos_w0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    Coefficients c;
    c.b0 = 1.0 + alpha * A;
    c.b1 = -2.0 * cos_w0;
    c.b2 = 1.0 - alpha * A;
    const double a0 = 1.0 + alpha / A;
    const double a1 = -2.0 * cos_w0;
    const double a2 = 1.0 - alpha / A;
    c.b0 /= a0;
    c.b1 /= a0;
    c.b2 /= a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

}  // namespace

float EqualizerFilter::Biquad::process(float sample) {
    const double x = sample;
    const double y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1;
    x1 = x;
    y2 = y1;
    y1 = y;
    return static_cast<float>(y);
}

bool VolumeFilter::configure(const AudioFormat& input, AudioFormat& output, std::string& error) {
    (void)error;
    output = input;
    return true;
}

bool VolumeFilter::process(audio::PcmBuffer& buffer, std::string& error) {
    (void)error;
    if (gain_ == 1.0f || buffer.samples.empty()) {
        return true;
    }
    audio::applyGain(buffer.samples.data(), buffer.samples.size(), gain_);
    return true;
}

void ResampleFilter::reset() {
    have_prev_ = false;
    position_ = 0.0;
    prev_.clear();
}

bool ResampleFilter::configure(const AudioFormat& input, AudioFormat& output, std::string& error) {
    if (target_rate_ == 0) {
        error = "target sample rate must be positive";
        return false;
    }
    if (input.sample_rate == 0 || input.channels == 0) {
        error = "input has no sample rate or channels";
        return false;
    }
    output = input;
    output.sample_rate = target_rate_;
    return true;
}

bool ResampleFilter::process(audio::PcmBuffer& buffer, std::string& error) {
    (void)error;
    const uint32_t input_rate = buffer.sample_rate;
    const uint32_t channels = buffer.channels;
    if (input_rate == 0 || channels == 0 || target_rate_ == 0 || input_rate == target_rate_) {
        return true;
    }
    const size_t input_frames = buffer.frames();
    if (input_frames == 0) {
        return true;
    }
    if (prev_.size() != channels) {
        prev_.assign(channels, 0.0f);
        have_prev_ = false;
        position_ = 0.0;
    }
    const double step = static_cast<double>(input_rate) / static_cast<double>(target_rate_);
    std::vector<float> out;
    out.reserve(static_cast<size_t>(std::ceil(static_cast<double>(input_frames) / step)) + 4);
    double position = position_;
    for (size_t frame = 0; frame < input_frames; ++frame) {
        const float* current = buffer.samples.data() + frame * channels;
        if (have_prev_) {
            // The segment between the previous and the current input sample
            // holds every output position in [0, 1); the first input sample of
            // a stream only becomes the left edge of the first segment.
            while (position < 1.0) {
                for (uint32_t channel = 0; channel < channels; ++channel) {
                    const float value =
                        prev_[channel] +
                        (current[channel] - prev_[channel]) * static_cast<float>(position);
                    out.push_back(value);
                }
                position += step;
            }
            position -= 1.0;
        }
        std::copy(current, current + channels, prev_.begin());
        have_prev_ = true;
    }
    position_ = position;
    buffer.samples = std::move(out);
    buffer.sample_rate = target_rate_;
    return true;
}

bool ChannelsFilter::configure(const AudioFormat& input, AudioFormat& output, std::string& error) {
    if (target_channels_ != 1 && target_channels_ != 2) {
        error = "only mono and stereo output are supported";
        return false;
    }
    if (input.channels != 1 && input.channels != 2) {
        error = "input must be mono or stereo";
        return false;
    }
    output = input;
    output.channels = target_channels_;
    return true;
}

bool ChannelsFilter::process(audio::PcmBuffer& buffer, std::string& error) {
    if (buffer.channels == target_channels_) {
        return true;
    }
    if (target_channels_ == 1 && buffer.channels == 2) {
        const size_t frames = buffer.frames();
        for (size_t frame = 0; frame < frames; ++frame) {
            buffer.samples[frame] = (buffer.samples[frame * 2] + buffer.samples[frame * 2 + 1]) * 0.5f;
        }
        buffer.samples.resize(frames);
        buffer.channels = 1;
        return true;
    }
    if (target_channels_ == 2 && buffer.channels == 1) {
        const size_t frames = buffer.frames();
        buffer.samples.resize(frames * 2);
        for (size_t frame = frames; frame-- > 0;) {
            const float value = buffer.samples[frame];
            buffer.samples[frame * 2] = value;
            buffer.samples[frame * 2 + 1] = value;
        }
        buffer.channels = 2;
        return true;
    }
    error = "unsupported channel layout";
    return false;
}

void EqualizerFilter::reset() {
    for (auto& channel : channels_) {
        for (auto& band : channel) {
            band.x1 = band.x2 = band.y1 = band.y2 = 0.0;
        }
    }
}

bool EqualizerFilter::configure(const AudioFormat& input, AudioFormat& output, std::string& error) {
    if (input.sample_rate == 0 || input.channels == 0) {
        error = "input has no sample rate or channels";
        return false;
    }
    output = input;
    bypass_ = bass_db_ == 0.0 && mid_db_ == 0.0 && treble_db_ == 0.0;
    if (bypass_) {
        channels_.clear();
        return true;
    }
    const double rate = static_cast<double>(input.sample_rate);
    const Coefficients low = lowShelf(rate, clampFrequency(rate, 200.0), bass_db_);
    const Coefficients mid = peaking(rate, clampFrequency(rate, 1000.0), 1.0, mid_db_);
    const Coefficients high = highShelf(rate, clampFrequency(rate, 6000.0), treble_db_);
    channels_.assign(input.channels, {});
    for (auto& channel : channels_) {
        channel[0].b0 = low.b0;
        channel[0].b1 = low.b1;
        channel[0].b2 = low.b2;
        channel[0].a1 = low.a1;
        channel[0].a2 = low.a2;
        channel[1].b0 = mid.b0;
        channel[1].b1 = mid.b1;
        channel[1].b2 = mid.b2;
        channel[1].a1 = mid.a1;
        channel[1].a2 = mid.a2;
        channel[2].b0 = high.b0;
        channel[2].b1 = high.b1;
        channel[2].b2 = high.b2;
        channel[2].a1 = high.a1;
        channel[2].a2 = high.a2;
    }
    return true;
}

bool EqualizerFilter::process(audio::PcmBuffer& buffer, std::string& error) {
    if (bypass_ || buffer.channels == 0 || buffer.samples.empty()) {
        return true;
    }
    if (channels_.size() != buffer.channels) {
        error = "not configured for this channel count";
        return false;
    }
    const size_t frames = buffer.frames();
    for (size_t frame = 0; frame < frames; ++frame) {
        float* samples = buffer.samples.data() + frame * buffer.channels;
        for (uint32_t channel = 0; channel < buffer.channels; ++channel) {
            auto& bands = channels_[channel];
            float value = samples[channel];
            value = bands[0].process(value);
            value = bands[1].process(value);
            value = bands[2].process(value);
            samples[channel] = value;
        }
    }
    return true;
}

}  // namespace player::filters
