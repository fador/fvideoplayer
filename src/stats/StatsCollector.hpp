#pragma once

#include "decode/KvzdecDecoder.hpp"

#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace player::stats {

struct StreamInfo {
    std::string container;
    std::string codec = "HEVC";
    std::string profile;
    std::string level;
    uint32_t width = 0;
    uint32_t height = 0;
    int bitdepth = 0;
    int chroma_format = 0;
    double fps = 0.0;
    double duration = 0.0;
};

struct FrameInfoSnapshot {
    bool valid = false;
    int64_t pts = 0;
    int32_t poc = 0;
    int qp = 0;
    int temporal_id = 0;
    decode::SliceType slice_type = decode::SliceType::Unknown;
    
    decode::HashResult hash_result = decode::HashResult::NotPresent;
    double decode_ms = 0.0;
    size_t sample_bytes = 0;
};

struct Snapshot {
    StreamInfo stream;
    FrameInfoSnapshot current;

    int64_t frames_decoded = 0;
    int64_t frames_displayed = 0;
    int64_t frames_dropped = 0;
    int64_t bytes_demuxed = 0;
    int64_t seeks = 0;
    double last_seek_ms = 0.0;

    double decode_fps = 0.0;
    double display_fps = 0.0;
    double avg_decode_ms = 0.0;
    double min_decode_ms = 0.0;
    double max_decode_ms = 0.0;
    double p95_decode_ms = 0.0;
    double bitrate_mbps = 0.0;

    size_t queue_depth = 0;
    size_t queue_capacity = 0;
    double buffered_seconds = 0.0;
    bool eos = false;
    bool playing = false;

    int64_t audio_buffer_ms = 0;
    int64_t audio_sink_ms = 0;
    int64_t audio_drift_ms = 0;
    int64_t audio_underruns = 0;

    std::vector<float> decode_history_ms;
    std::vector<float> queue_history;
    std::vector<float> bitrate_history_mbps;
};

// Thread-safe collection of decoder and playback statistics. The pipeline
// thread writes, the UI thread takes periodic snapshots.
class StatsCollector {
public:
    void reset();
    void setStreamInfo(const StreamInfo& info);
    void onFrameDecoded(const decode::DecodedFrame& frame, double decode_ms);
    void onFrameDisplayed(bool dropped);
    void onBytesDemuxed(size_t bytes);
    void onSeek(double duration_ms);
    void setQueue(size_t depth, size_t capacity, double buffered_seconds);
    void setAudio(int64_t buffer_ms, int64_t sink_ms, int64_t drift_ms, int64_t underruns);
    void setPlaying(bool playing);
    void setEos(bool eos);

    Snapshot snapshot() const;

private:
    mutable std::mutex mutex_;
    StreamInfo stream_;
    FrameInfoSnapshot current_;
    int64_t frames_decoded_ = 0;
    int64_t frames_displayed_ = 0;
    int64_t frames_dropped_ = 0;
    int64_t bytes_demuxed_ = 0;
    int64_t seeks_ = 0;
    double last_seek_ms_ = 0.0;
    size_t queue_depth_ = 0;
    size_t queue_capacity_ = 0;
    double buffered_seconds_ = 0.0;
    int64_t audio_buffer_ms_ = 0;
    int64_t audio_sink_ms_ = 0;
    int64_t audio_drift_ms_ = 0;
    int64_t audio_underruns_ = 0;
    bool eos_ = false;
    bool playing_ = false;

    std::deque<float> decode_ms_history_;   // last N decode times
    std::deque<double> decode_times_;       // steady-clock seconds of recent frames
    std::deque<double> display_times_;
    std::deque<std::pair<double, size_t>> byte_events_;
    mutable std::deque<float> queue_history_;
    mutable std::deque<float> bitrate_history_;
};

}  // namespace player::stats
