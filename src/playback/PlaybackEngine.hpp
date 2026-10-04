#pragma once

#include "audio/AudioBuffer.hpp"
#include "audio/IAudioSink.hpp"
#include "audio/NullAudioSink.hpp"
#include "audio/Pcm.hpp"
#include "audio/PcmGain.hpp"
#include "audio/aac/AacDecoder.hpp"
#include "audio/flac/FlacDecoder.hpp"
#include "audio/mp2/Mp2Decoder.hpp"
#include "audio/mp3/Mp3Decoder.hpp"
#include "audio/opus/OpusDecoder.hpp"
#include "decode/KvzdecDecoder.hpp"
#include "decode/mpeg12/Mpeg12Decoder.hpp"
#include "demux/Demuxer.hpp"
#include "filters/Filter.hpp"
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
    // True when the selected track is decodable and the decoder accepted it.
    bool audioDecoding() const { return audio_decoding_.load(); }
    // Name of the codec the audio decoder is configured for.
    const char* audioCodecName() const;

    // Post-decode filter graphs in ffmpeg-style syntax, e.g.
    // "scale=1280:720,flip=h" and "volume=1.5,equalizer=bass=6". An invalid
    // spec is rejected and the current graph stays in place.
    bool setVideoFilters(const std::string& spec, std::string& error);
    bool setAudioFilters(const std::string& spec, std::string& error);
    std::string videoFilterSpec() const;
    std::string audioFilterSpec() const;
    std::vector<std::string> videoFilterNames() const;
    std::vector<std::string> audioFilterNames() const;

    const std::string& lastError() const { return last_error_; }
    stats::StatsCollector& stats() { return stats_; }

private:
    enum class AudioCodec {
        None,
        Aac,
        Opus,
        Mp2,
        Mp3,
        Flac,
    };

    // Video backend selected from the container's codec id.
    enum class VideoCodec {
        None,
        Hevc,
        Mpeg1,
        Mpeg2,
    };

    //    Video: one sample is fed to the backend selected by the container and
    //    the pictures it produces are pulled in display order.
    bool configureVideoDecoder(std::string& error);
    void resetVideoDecoder();
    bool pushVideoSample(const demux::Sample& sample);
    int pullVideoFrames(std::vector<decode::DecodedFrame>& out);
    int flushVideoDecoder(std::vector<decode::DecodedFrame>& out);
    static const char* videoCodecName(demux::Codec codec);


    //    Audio scheduling: decoded PCM goes into audio_buffer_ up to
    //    kAudioBufferMs ahead of the media clock; feedAudio() moves chunks
    //    into the sink so the sink queue stays around kAudioSinkLeadMs.
    //    Chunks that are more than kAudioLateMs behind are dropped.
    static constexpr int kAudioBufferMs = 2000;
    static constexpr int kAudioSinkLeadMs = 400;
    static constexpr int kAudioLateMs = 120;

    // Post-decode filter graph. The chains run on the pipeline thread; the
    // UI thread only ever swaps the shared_ptr under filter_mutex_.
    bool applyVideoFilters(decode::DecodedFrame& frame);
    bool applyAudioFilters(audio::PcmBuffer& chunk);
    void rebuildFilters();
    void resetFilterState();

    void pipelineLoop();
    void performSeek(double seconds);
    void configureAudioDecoder(const demux::AudioTrackInfo& track);
    void decodeAudioSample(const demux::Sample& sample);
    void flushAudioDecoder();
    void resetAudioDecoder();
    void pumpAudio();
    void feedAudio();
    void resetAudioState();
    int64_t positionMs() const {
        return static_cast<int64_t>(clock_.position() * 1000.0);
    }
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
    VideoCodec video_codec_ = VideoCodec::None;
    decode::KvzdecDecoder decoder_;
    decode::mpeg12::Decoder mpeg12_decoder_;
    audio::IAudioSink* audio_sink_ = nullptr;
    audio::NullAudioSink null_sink_;
    AudioCodec audio_codec_ = AudioCodec::None;
    audio::aac::Decoder aac_decoder_;
    audio::opus::Decoder opus_decoder_;
    audio::mp2::Decoder mp2_decoder_;
    audio::mp3::Decoder mp3_decoder_;
    audio::flac::Decoder flac_decoder_;
    audio::AudioBuffer audio_buffer_{kAudioBufferMs};
    audio::PcmBuffer audio_pcm_;
    mutable std::mutex filter_mutex_;
    std::string video_filter_spec_;
    std::string audio_filter_spec_;
    std::shared_ptr<filters::VideoFilterChain> video_filters_;
    std::shared_ptr<filters::AudioFilterChain> audio_filters_;
    std::atomic<bool> video_filter_warned_{false};
    std::atomic<bool> audio_filter_warned_{false};
    uint32_t audio_out_rate_ = 0;
    uint32_t audio_out_channels_ = 0;
    uint32_t audio_out_fourcc_ = 0;
    std::atomic<float> volume_{1.0f};
    std::atomic<bool> muted_{false};
    std::atomic<bool> audio_decoding_{false};
    std::atomic<bool> audio_eos_{false};
    int audio_frame_ms_ = 21;
    uint64_t audio_late_drops_ = 0;
    bool audio_warned_ = false;
    std::mutex audio_mutex_;  // serializes feeding with seek/track resets

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


