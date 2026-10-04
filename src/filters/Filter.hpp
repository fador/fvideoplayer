#pragma once

#include "audio/Pcm.hpp"
#include "decode/Frame.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace player::filters {

// Geometry a video filter sees at its input. Matches the planar layout of
// decode::Frame; a filter that resizes or crops writes new dimensions to the
// output format so the next stage reconfigures.
struct VideoFormat {
    int width = 0;
    int height = 0;
    decode::ChromaFormat chroma = decode::ChromaFormat::C420;
    int bitdepth = 8;
    int bitdepth_c = 8;
};

inline bool operator==(const VideoFormat& a, const VideoFormat& b) {
    return a.width == b.width && a.height == b.height && a.chroma == b.chroma &&
           a.bitdepth == b.bitdepth && a.bitdepth_c == b.bitdepth_c;
}

inline bool operator!=(const VideoFormat& a, const VideoFormat& b) {
    return !(a == b);
}

VideoFormat videoFormatOf(const decode::Frame& frame);

// One stage of the post-decode video graph, the equivalent of a DirectShow
// transform filter. Filters run on the pipeline thread, in chain order, before
// a frame reaches the renderer.
class IVideoFilter {
public:
    virtual ~IVideoFilter() = default;

    virtual const char* name() const = 0;

    // Reports the format this filter produces for the given input. Returning
    // false rejects the stream with a reason.
    virtual bool configure(const VideoFormat& input, VideoFormat& output, std::string& error) = 0;

    // Transforms the frame in place, replacing it when the geometry changes.
    virtual bool process(decode::Frame& frame, std::string& error) = 0;

    // Clears inter-frame state; called on seek and when the chain is rebuilt.
    virtual void reset() {}
};

class VideoFilterChain {
public:
    void add(std::unique_ptr<IVideoFilter> filter);
    bool empty() const { return filters_.empty(); }
    size_t size() const { return filters_.size(); }
    std::vector<std::string> names() const;

    // Runs every filter in order, reconfiguring the chain when the input
    // format changes.
    bool process(decode::Frame& frame, std::string& error);
    void reset();

private:
    std::vector<std::unique_ptr<IVideoFilter>> filters_;
    bool configured_ = false;
    VideoFormat input_;
};

// Sample layout a decoded PCM chunk carries. A filter that resamples or
// remixes replaces it for the rest of the chain and the sink.
struct AudioFormat {
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
};

inline bool operator==(const AudioFormat& a, const AudioFormat& b) {
    return a.sample_rate == b.sample_rate && a.channels == b.channels;
}

inline bool operator!=(const AudioFormat& a, const AudioFormat& b) {
    return !(a == b);
}

// One stage of the post-decode audio graph. Buffers are interleaved float.
class IAudioFilter {
public:
    virtual ~IAudioFilter() = default;

    virtual const char* name() const = 0;

    virtual bool configure(const AudioFormat& input, AudioFormat& output, std::string& error) = 0;

    // Transforms one decoded chunk, in place.
    virtual bool process(audio::PcmBuffer& buffer, std::string& error) = 0;

    virtual void reset() {}
};

class AudioFilterChain {
public:
    void add(std::unique_ptr<IAudioFilter> filter);
    bool empty() const { return filters_.empty(); }
    size_t size() const { return filters_.size(); }
    std::vector<std::string> names() const;

    bool process(audio::PcmBuffer& buffer, std::string& error);
    void reset();

private:
    std::vector<std::unique_ptr<IAudioFilter>> filters_;
    bool configured_ = false;
    AudioFormat input_;
};

}  // namespace player::filters
