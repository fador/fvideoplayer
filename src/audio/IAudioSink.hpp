#pragma once

#include <cstddef>
#include <cstdint>

namespace player::audio {

struct AudioFormat {
    uint32_t codec_fourcc = 0;
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
};

// Audio output abstraction. Samples are interleaved 32-bit floats in [-1, 1];
// the sink owns conversion to the device format and playback timing.
class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    virtual void configure(const AudioFormat& format) { (void)format; }
    virtual void push(const float* samples, size_t frame_count) {
        (void)samples;
        (void)frame_count;
    }
    virtual void pause() {}
    virtual void resume() {}
    // Drops queued audio without touching counters; used on seek.
    virtual void flush() {}
    // Drops queued audio and resets counters; used on track change/close.
    virtual void reset() {}

    virtual uint64_t framesConsumed() const { return 0; }
    virtual uint64_t framesDropped() const { return 0; }
    // Playback delay of the queued audio in milliseconds.
    virtual int queuedMs() const { return 0; }
    // Times the device queue ran dry while playing (diagnostic).
    virtual uint64_t underruns() const { return 0; }
};

}  // namespace player::audio
