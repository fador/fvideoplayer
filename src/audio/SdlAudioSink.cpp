#include "audio/SdlAudioSink.hpp"

#include "utils/Log.hpp"

#include <SDL3/SDL.h>

namespace player::audio {

SdlAudioSink::~SdlAudioSink() {
    close();
}

void SdlAudioSink::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closeStream();
}

void SdlAudioSink::configure(const AudioFormat& format) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (format.channels == 0 || format.sample_rate == 0) {
        return;
    }
    if (stream_ && format.channels == channels_ && format.sample_rate == sample_rate_) {
        return;
    }
    closeStream();
    channels_ = format.channels;
    sample_rate_ = format.sample_rate;
}

bool SdlAudioSink::ensureStream() {
    if (stream_) {
        return true;
    }
    if (channels_ == 0 || sample_rate_ == 0) {
        return false;
    }
    if (!audio_initialized_) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            log::addf(log::Level::Error, "Audio subsystem init failed: %s", SDL_GetError());
            return false;
        }
        audio_initialized_ = true;
    }

    SDL_AudioSpec spec{};
    spec.format = SDL_AUDIO_F32;
    spec.channels = static_cast<int>(channels_);
    spec.freq = static_cast<int>(sample_rate_);
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (!stream_) {
        log::addf(log::Level::Error, "Audio device open failed: %s", SDL_GetError());
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_initialized_ = false;
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    paused_ = false;

    // The device keeps its own buffer, which SDL_GetAudioStreamQueued does
    // not report; include it so the engine's A/V scheduling accounts for the
    // full output latency.
    device_latency_ms_ = 0;
    const SDL_AudioDeviceID device = SDL_GetAudioStreamDevice(stream_);
    SDL_AudioSpec device_spec{};
    int sample_frames = 0;
    if (device != 0 && SDL_GetAudioDeviceFormat(device, &device_spec, &sample_frames) &&
        device_spec.freq > 0) {
        device_latency_ms_ = sample_frames * 1000 / device_spec.freq;
    }
    log::addf(log::Level::Info, "Audio output: %u Hz, %u channel(s), float32, %d ms device buffer",
              sample_rate_, channels_, device_latency_ms_);
    return true;
}

void SdlAudioSink::closeStream() {
    if (stream_) {
        SDL_DestroyAudioStream(stream_);
        stream_ = nullptr;
    }
    if (audio_initialized_) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_initialized_ = false;
    }
    device_latency_ms_ = 0;
    paused_ = true;
}

void SdlAudioSink::push(const float* samples, size_t frame_count) {
    if (!samples || frame_count == 0) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!ensureStream()) {
        frames_dropped_ += frame_count;
        return;
    }

    const int bytes_per_frame = static_cast<int>(channels_ * sizeof(float));
    const int queued = SDL_GetAudioStreamQueued(stream_);
    const int limit = kMaxQueuedMs * static_cast<int>(sample_rate_) / 1000 * bytes_per_frame;
    if (queued >= limit) {
        frames_dropped_ += frame_count;
        return;
    }

    // Clip to the remaining headroom instead of overshooting the bound.
    const size_t room = static_cast<size_t>(limit - queued) / static_cast<size_t>(bytes_per_frame);
    if (frame_count > room) {
        frames_dropped_ += frame_count - room;
        frame_count = room;
    }
    if (frame_count == 0) {
        return;
    }
    const int bytes = static_cast<int>(frame_count) * bytes_per_frame;
    if (!SDL_PutAudioStreamData(stream_, samples, bytes)) {
        log::addf(log::Level::Warning, "Audio queue push failed: %s", SDL_GetError());
        frames_dropped_ += frame_count;
        return;
    }
    frames_consumed_ += frame_count;
}

void SdlAudioSink::pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_ || paused_) {
        return;
    }
    SDL_PauseAudioStreamDevice(stream_);
    paused_ = true;
}

void SdlAudioSink::resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_ || !paused_) {
        return;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    paused_ = false;
}

void SdlAudioSink::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_) {
        SDL_ClearAudioStream(stream_);
    }
}

void SdlAudioSink::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (stream_) {
        SDL_ClearAudioStream(stream_);
    }
    frames_consumed_ = 0;
    frames_dropped_ = 0;
}

uint64_t SdlAudioSink::framesConsumed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_consumed_;
}

uint64_t SdlAudioSink::framesDropped() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frames_dropped_;
}

int SdlAudioSink::queuedMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!stream_ || channels_ == 0 || sample_rate_ == 0) {
        return 0;
    }
    const int bytes_per_frame = static_cast<int>(channels_ * sizeof(float));
    const int queued = SDL_GetAudioStreamQueued(stream_);
    return device_latency_ms_ + queued * 1000 / (bytes_per_frame * static_cast<int>(sample_rate_));
}

}  // namespace player::audio
