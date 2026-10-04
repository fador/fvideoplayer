#include "filters/Filter.hpp"

namespace player::filters {

VideoFormat videoFormatOf(const decode::Frame& frame) {
    VideoFormat format;
    format.width = frame.width_;
    format.height = frame.height_;
    format.chroma = frame.format_;
    format.bitdepth = frame.bitdepth_;
    format.bitdepth_c = frame.bitdepth_c_;
    return format;
}

void VideoFilterChain::add(std::unique_ptr<IVideoFilter> filter) {
    if (filter) {
        filters_.push_back(std::move(filter));
    }
}

std::vector<std::string> VideoFilterChain::names() const {
    std::vector<std::string> result;
    result.reserve(filters_.size());
    for (const auto& filter : filters_) {
        result.emplace_back(filter->name());
    }
    return result;
}

bool VideoFilterChain::process(decode::Frame& frame, std::string& error) {
    if (filters_.empty() || !frame.valid()) {
        return true;
    }
    const VideoFormat input = videoFormatOf(frame);
    if (!configured_ || input != input_) {
        VideoFormat format = input;
        for (const auto& filter : filters_) {
            if (!filter->configure(format, format, error)) {
                configured_ = false;
                error = std::string(filter->name()) + ": " + error;
                return false;
            }
        }
        configured_ = true;
        input_ = input;
    }
    for (const auto& filter : filters_) {
        if (!filter->process(frame, error)) {
            error = std::string(filter->name()) + ": " + error;
            return false;
        }
    }
    return true;
}

void VideoFilterChain::reset() {
    configured_ = false;
    input_ = VideoFormat{};
    for (const auto& filter : filters_) {
        filter->reset();
    }
}

void AudioFilterChain::add(std::unique_ptr<IAudioFilter> filter) {
    if (filter) {
        filters_.push_back(std::move(filter));
    }
}

std::vector<std::string> AudioFilterChain::names() const {
    std::vector<std::string> result;
    result.reserve(filters_.size());
    for (const auto& filter : filters_) {
        result.emplace_back(filter->name());
    }
    return result;
}

bool AudioFilterChain::process(audio::PcmBuffer& buffer, std::string& error) {
    if (filters_.empty() || buffer.samples.empty()) {
        return true;
    }
    const AudioFormat input{buffer.sample_rate, buffer.channels};
    if (!configured_ || input != input_) {
        AudioFormat format = input;
        for (const auto& filter : filters_) {
            filter->reset();
            if (!filter->configure(format, format, error)) {
                configured_ = false;
                error = std::string(filter->name()) + ": " + error;
                return false;
            }
        }
        configured_ = true;
        input_ = input;
    }
    for (const auto& filter : filters_) {
        if (!filter->process(buffer, error)) {
            error = std::string(filter->name()) + ": " + error;
            return false;
        }
    }
    return true;
}

void AudioFilterChain::reset() {
    configured_ = false;
    input_ = AudioFormat{};
    for (const auto& filter : filters_) {
        filter->reset();
    }
}

}  // namespace player::filters
