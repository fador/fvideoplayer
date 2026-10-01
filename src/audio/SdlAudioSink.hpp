#pragma once

#include "audio/IAudioSink.hpp"

#include <SDL3/SDL_audio.h>

#include <cstdint>
#include <mutex>

namespace player::audio {

// SDL3 playback sink for interleaved float PCM. SDL converts and resamples to
// the device format. The queue is bounded so a fast decoder cannot run seconds
// ahead of the video clock; excess PCM is dropped and counted.
class SdlAudioSink final : public IAudioSink {
public:
    SdlAudioSink() = default;
    ~SdlAudioSink() override;

    SdlAudioSink(const SdlAudioSink&) = delete;
    SdlAudioSink& operator=(const SdlAudioSink&) = delete;

    // Closes the device; must run before SDL_Quit().
    void close();

    void configure(const AudioFormat& format) override;
    void push(const float* samples, size_t frame_count) override;
    void pause() override;
    void resume() override;
    void flush() override;
    void reset() override;

    uint64_t framesConsumed() const override;
    uint64_t framesDropped() const override;
    int queuedMs() const override;
    bool open() const { return stream_ != nullptr; }

private:
    bool ensureStream();
    void closeStream();

    static constexpr int kMaxQueuedMs = 250;

    mutable std::mutex mutex_;
    SDL_AudioStream* stream_ = nullptr;
    uint32_t channels_ = 0;
    uint32_t sample_rate_ = 0;
    bool audio_initialized_ = false;
    bool paused_ = true;
    uint64_t frames_consumed_ = 0;
    uint64_t frames_dropped_ = 0;
};

}  // namespace player::audio
