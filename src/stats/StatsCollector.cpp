#include "stats/StatsCollector.hpp"

#include <algorithm>
#include <chrono>

namespace player::stats {
namespace {

constexpr size_t kDecodeHistory = 600;
constexpr double kRateWindowSeconds = 1.0;
constexpr double kByteWindowSeconds = 1.0;

double nowSeconds() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void trimTimes(std::deque<double>& times, double window) {
    const double cutoff = nowSeconds() - window;
    while (!times.empty() && times.front() < cutoff) {
        times.pop_front();
    }
}

}  // namespace

void StatsCollector::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    stream_ = StreamInfo{};
    current_ = FrameInfoSnapshot{};
    frames_decoded_ = 0;
    frames_displayed_ = 0;
    frames_dropped_ = 0;
    bytes_demuxed_ = 0;
    seeks_ = 0;
    last_seek_ms_ = 0.0;
    queue_depth_ = 0;
    queue_capacity_ = 0;
    buffered_seconds_ = 0.0;
    audio_buffer_ms_ = 0;
    audio_sink_ms_ = 0;
    audio_drift_ms_ = 0;
    audio_underruns_ = 0;
    eos_ = false;
    playing_ = false;
    decode_ms_history_.clear();
    decode_times_.clear();
    display_times_.clear();
    byte_events_.clear();
}

void StatsCollector::setStreamInfo(const StreamInfo& info) {
    std::lock_guard<std::mutex> lock(mutex_);
    stream_ = info;
}

void StatsCollector::onFrameDecoded(const decode::DecodedFrame& frame, double decode_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++frames_decoded_;
    decode_times_.push_back(nowSeconds());
    trimTimes(decode_times_, kRateWindowSeconds);

    decode_ms_history_.push_back(static_cast<float>(decode_ms));
    while (decode_ms_history_.size() > kDecodeHistory) {
        decode_ms_history_.pop_front();
    }

    current_.valid = true;
    current_.pts = frame.pts;
    current_.poc = frame.info.poc;
    current_.qp = frame.info.qp;
    current_.temporal_id = frame.info.temporal_id;
    current_.slice_type = static_cast<int>(frame.info.slice_type);
    current_.nal_unit_type = static_cast<int>(frame.info.nal_unit_type);
    current_.hash_result = frame.info.hash_result;
    current_.decode_ms = decode_ms;
    current_.sample_bytes = frame.sample_bytes;
}

void StatsCollector::onFrameDisplayed(bool dropped) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (dropped) {
        ++frames_dropped_;
    } else {
        ++frames_displayed_;
    }
    display_times_.push_back(nowSeconds());
    trimTimes(display_times_, kRateWindowSeconds);
}

void StatsCollector::onBytesDemuxed(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    bytes_demuxed_ += static_cast<int64_t>(bytes);
    byte_events_.emplace_back(nowSeconds(), bytes);
    const double cutoff = nowSeconds() - kByteWindowSeconds;
    while (!byte_events_.empty() && byte_events_.front().first < cutoff) {
        byte_events_.pop_front();
    }
}

void StatsCollector::onSeek(double duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++seeks_;
    last_seek_ms_ = duration_ms;
}

void StatsCollector::setQueue(size_t depth, size_t capacity, double buffered_seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_depth_ = depth;
    queue_capacity_ = capacity;
    buffered_seconds_ = buffered_seconds;
}

void StatsCollector::setAudio(int64_t buffer_ms, int64_t sink_ms, int64_t drift_ms,
                              int64_t underruns) {
    std::lock_guard<std::mutex> lock(mutex_);
    audio_buffer_ms_ = buffer_ms;
    audio_sink_ms_ = sink_ms;
    audio_drift_ms_ = drift_ms;
    audio_underruns_ = underruns;
}

void StatsCollector::setPlaying(bool playing) {
    std::lock_guard<std::mutex> lock(mutex_);
    playing_ = playing;
}

void StatsCollector::setEos(bool eos) {
    std::lock_guard<std::mutex> lock(mutex_);
    eos_ = eos;
}

Snapshot StatsCollector::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Snapshot out;
    out.stream = stream_;
    out.current = current_;
    out.frames_decoded = frames_decoded_;
    out.frames_displayed = frames_displayed_;
    out.frames_dropped = frames_dropped_;
    out.bytes_demuxed = bytes_demuxed_;
    out.seeks = seeks_;
    out.last_seek_ms = last_seek_ms_;
    out.queue_depth = queue_depth_;
    out.queue_capacity = queue_capacity_;
    out.buffered_seconds = buffered_seconds_;
    out.audio_buffer_ms = audio_buffer_ms_;
    out.audio_sink_ms = audio_sink_ms_;
    out.audio_drift_ms = audio_drift_ms_;
    out.audio_underruns = audio_underruns_;
    out.eos = eos_;
    out.playing = playing_;

    const double now = nowSeconds();
    const double rate_cutoff = now - kRateWindowSeconds;
    out.decode_fps = static_cast<double>(
        std::count_if(decode_times_.begin(), decode_times_.end(),
                      [rate_cutoff](double t) { return t >= rate_cutoff; }));
    out.display_fps = static_cast<double>(
        std::count_if(display_times_.begin(), display_times_.end(),
                      [rate_cutoff](double t) { return t >= rate_cutoff; }));

    size_t bytes = 0;
    const double byte_cutoff = now - kByteWindowSeconds;
    for (const auto& event : byte_events_) {
        if (event.first >= byte_cutoff) {
            bytes += event.second;
        }
    }
    out.bitrate_mbps = static_cast<double>(bytes) * 8.0 / 1e6 / kByteWindowSeconds;

    if (!decode_ms_history_.empty()) {
        double sum = 0.0;
        double min_value = decode_ms_history_.front();
        double max_value = decode_ms_history_.front();
        for (float value : decode_ms_history_) {
            sum += value;
            min_value = std::min(min_value, static_cast<double>(value));
            max_value = std::max(max_value, static_cast<double>(value));
        }
        out.avg_decode_ms = sum / static_cast<double>(decode_ms_history_.size());
        out.min_decode_ms = min_value;
        out.max_decode_ms = max_value;

        std::vector<float> sorted(decode_ms_history_.begin(), decode_ms_history_.end());
        std::sort(sorted.begin(), sorted.end());
        const size_t index = static_cast<size_t>(0.95 * static_cast<double>(sorted.size() - 1));
        out.p95_decode_ms = sorted[index];
        out.decode_history_ms.assign(decode_ms_history_.begin(), decode_ms_history_.end());
    }

    queue_history_.push_back(static_cast<float>(buffered_seconds_));
    while (queue_history_.size() > 300) {
        queue_history_.pop_front();
    }
    out.queue_history.assign(queue_history_.begin(), queue_history_.end());

    bitrate_history_.push_back(static_cast<float>(out.bitrate_mbps));
    while (bitrate_history_.size() > 300) {
        bitrate_history_.pop_front();
    }
    out.bitrate_history_mbps.assign(bitrate_history_.begin(), bitrate_history_.end());
    return out;
}

}  // namespace player::stats
