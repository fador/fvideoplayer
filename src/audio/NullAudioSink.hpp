#pragma once

#include "audio/IAudioSink.hpp"

namespace player::audio {

// Discards audio packets but keeps counters so the UI can show that audio
// data is being demuxed. Replacing this with a real sink is the only change
// needed to enable audio output.
class NullAudioSink final : public IAudioSink {
public:
    void push(const demux::Sample& sample) override {
        ++packets_;
        bytes_ += sample.data.size();
    }

    void reset() override {
        packets_ = 0;
        bytes_ = 0;
    }

    uint64_t bytesConsumed() const override { return bytes_; }
    uint64_t packetsConsumed() const override { return packets_; }

private:
    uint64_t packets_ = 0;
    uint64_t bytes_ = 0;
};

}  // namespace player::audio
