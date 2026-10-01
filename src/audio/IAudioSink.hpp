#pragma once

#include "demux/Demuxer.hpp"

#include <cstdint>

namespace player::audio {

struct AudioFormat {
    uint32_t codec_fourcc = 0;
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
};

// Audio output abstraction. Real audio output will be added later; for now a
// null sink consumes (and counts) the audio packets.
class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    virtual void configure(const AudioFormat& format) { (void)format; }
    virtual void push(const demux::Sample& sample) { (void)sample; }
    virtual void flush() {}
    virtual void reset() {}
    virtual uint64_t bytesConsumed() const { return 0; }
    virtual uint64_t packetsConsumed() const { return 0; }
};

}  // namespace player::audio
