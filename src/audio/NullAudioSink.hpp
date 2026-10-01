#pragma once

#include "audio/IAudioSink.hpp"

namespace player::audio {

// Discards PCM but keeps counters, so the pipeline and the UI work without an
// audio device. Used as fallback when no real sink is attached.
class NullAudioSink final : public IAudioSink {
public:
    void push(const float* samples, size_t frame_count) override {
        (void)samples;
        frames_ += frame_count;
    }

    void reset() override { frames_ = 0; }

    uint64_t framesConsumed() const override { return frames_; }

private:
    uint64_t frames_ = 0;
};

}  // namespace player::audio
