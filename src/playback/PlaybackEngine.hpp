#pragma once

#include "audio/IAudioSink.hpp"
#include "audio/NullAudioSink.hpp"
#include "audio/Pcm.hpp"
#include "audio/PcmGain.hpp"
#include "audio/aac/AacDecoder.hpp"
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
    // Increments whenever the displayed frame changes, even if a new
    // DecodedFrame happens to reuse the previous allocation address.
    uint64_t currentSerial() const { return current_serial_; }
    // Bit depth parsed from the stream's hvcC (0 if unknown).
    int streamBitDepth() const { return stream_bitdepth_; }
    bool buffering() const;

    static constexpr size_t kNoAudioTrack = static_cast<size_t>(-1);

    const std::vector<demux::AudioTrackInfo>& audioTracks() const { return audio_track_infos_; }
    const std::vector<demux::SubtitleTrackInfo>& subtitleTracks() const {
        return subtitle_track_infos_;
    }
    void selectAudioTrack(size_t index);
    size_t selectedAudioTrack() const { return selected_audio_.load(); }
    bool audioEnabled() const { return audio_enabled_.load(); }
    // Reads and parses an embedded subtitle track (safe to call from the UI
    // thread; uses a private file handle).
    bool loadSubtitleTrack(size_t index, std::vector<subtitle::SubtitleBlock>& blocks,
                           std::string& error);
    // Audio output is injected by the app (SDL sink in the GUI); without a
    // sink PCM is discarded through an internal null sink.
    void setAudioSink(audio::IAudioSink* sink) { audio_sink_ = sink; }
    void setVolume(float volume) { volume_ = audio::clampVolume(volume); }
    float volume() const { return volume_.load(); }
    void setMuted(audio::Mute mute) { muted_ = mute == audio::Mute::On; }
    audio::Mute muted() const { return muted_.load() ? audio::Mute::On : audio::Mute::Off; }
    uint64_t audioFrames() { return sink().framesConsumed(); }
    uint64_t audioFramesDropped() { return sink().framesDropped(); }
    // True when the selected track is AAC and the decoder accepted its config.
    bool audioDecoding() const { return audio_decoding_.load(); }

    const std::string& lastError() const { return last_error_; }
    stats::StatsCollector& stats() { return stats_; }

private:
    void pipelineLoop();
    void performSeek(double seconds);
    void configureAudioDecoder(const demux::AudioTrackInfo& track);
    void pushAudioPcm(const demux::Sample& sample);
    audio::IAudioSink& sink() {
        return audio_sink_ ? *audio_sink_ : null_sink_;
    }

    std::thread thread_;
    std::atomic<bool> quit_{false};
    std::atomic<bool> eos_{false};
    std::atomic<State> state_{State::Empty};

    std::mutex mutex_;
    std::condition_variable cv_;
    bool seek_pending_ = false;
    double seek_target_ = 0.0;
    bool audio_pending_ = false;
    size_t audio_target_ = 0;

    // Pipeline-thread state.
    std::unique_ptr<demux::IDemuxer> demuxer_;
    decode::KvzdecDecoder decoder_;
    audio::IAudioSink* audio_sink_ = nullptr;
    audio::NullAudioSink null_sink_;
    audio::aac::Decoder audio_decoder_;
    audio::PcmBuffer audio_pcm_;
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> audio_decoding_{false};
    bool audio_warned_ = false;

    // UI-thread state.
    FrameQueue queue_{16};
    MediaClock clock_;
    std::unique_ptr<decode::DecodedFrame> current_;

    demux::VideoTrackInfo video_info_;
    demux::AudioTrackInfo audio_info_;
    double duration_ = 0.0;
    uint64_t current_serial_ = 0;
    int stream_bitdepth_ = 0;
    std::vector<demux::AudioTrackInfo> audio_track_infos_;
    std::vector<demux::SubtitleTrackInfo> subtitle_track_infos_;
    std::atomic<size_t> selected_audio_{0};
    std::atomic<bool> audio_enabled_{false};
    std::string last_error_;
    stats::StatsCollector stats_;
};

}  // namespace player::playback
