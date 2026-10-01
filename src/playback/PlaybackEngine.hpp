#pragma once

#include "audio/NullAudioSink.hpp"
#include "decode/KvzdecDecoder.hpp"
#include "demux/Demuxer.hpp"
#include "playback/FrameQueue.hpp"
#include "playback/MediaClock.hpp"
#include "stats/StatsCollector.hpp"

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace player::playback {

enum class State {
    Empty,
    Paused,
    Playing,
    Ended,
};

// Demux + decode pipeline with a bounded decode-ahead buffer.
//
// Threading: one pipeline thread reads samples and feeds the decoder; the UI
// thread advances the clock and pops due frames. The demuxer and decoder are
// only touched by the pipeline thread; open()/close() synchronize with it.
class PlaybackEngine {
public:
    PlaybackEngine();
    ~PlaybackEngine();

    PlaybackEngine(const PlaybackEngine&) = delete;
    PlaybackEngine& operator=(const PlaybackEngine&) = delete;

    bool open(const std::filesystem::path& path, std::string& error);
    void close();

    bool hasFile() const { return state_.load() != State::Empty; }
    const demux::VideoTrackInfo& videoInfo() const { return video_info_; }
    const demux::AudioTrackInfo& audioInfo() const { return audio_info_; }
    double duration() const { return duration_; }
    double position() const { return clock_.position(); }
    State state() const { return state_.load(); }

    void play();
    void pause();
    void togglePlayPause();
    void stop();
    void seek(double seconds);

    // UI thread: advances the clock and picks up due frames. Returns true when
    // the displayed frame changed.
    bool update();

    const decode::DecodedFrame* currentFrame() const { return current_.get(); }
    bool buffering() const;
    const std::string& lastError() const { return last_error_; }
    stats::StatsCollector& stats() { return stats_; }
    const audio::NullAudioSink& audioSink() const { return audio_sink_; }

private:
    void pipelineLoop();
    void performSeek(double seconds);

    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> eos_{false};
    std::atomic<State> state_{State::Empty};

    std::mutex mutex_;
    std::condition_variable cv_;
    bool seek_pending_ = false;
    double seek_target_ = 0.0;

    // Pipeline-thread state.
    std::unique_ptr<demux::IDemuxer> demuxer_;
    decode::KvzdecDecoder decoder_;
    audio::NullAudioSink audio_sink_;

    // UI-thread state.
    FrameQueue queue_{16};
    MediaClock clock_;
    std::unique_ptr<decode::DecodedFrame> current_;

    demux::VideoTrackInfo video_info_;
    demux::AudioTrackInfo audio_info_;
    double duration_ = 0.0;
    std::string last_error_;
    stats::StatsCollector stats_;
};

}  // namespace player::playback
