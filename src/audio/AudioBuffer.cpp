#include "audio/AudioBuffer.hpp"

namespace player::audio {

AudioBuffer::AudioBuffer(int64_t capacity_ms) : capacity_ms_(capacity_ms) {}

int64_t AudioBuffer::chunkDurationMs(const PcmBuffer& chunk) const {
    if (chunk.sample_rate == 0) {
        return 0;
    }
    return static_cast<int64_t>(chunk.frames()) * 1000 / static_cast<int64_t>(chunk.sample_rate);
}

void AudioBuffer::push(PcmBuffer&& chunk) {
    if (chunk.samples.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t duration = chunkDurationMs(chunk);

    // Keep the buffered duration within the capacity by dropping old chunks.
    while (!chunks_.empty() && duration_ms_ + duration > capacity_ms_) {
        duration_ms_ -= chunkDurationMs(chunks_.front());
        chunks_.pop_front();
        ++dropped_;
    }
    duration_ms_ += duration;
    end_pts_ms_ = chunk.pts + duration;
    chunks_.push_back(std::move(chunk));
}

bool AudioBuffer::pop(PcmBuffer& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (chunks_.empty()) {
        return false;
    }
    out = std::move(chunks_.front());
    duration_ms_ -= chunkDurationMs(out);
    chunks_.pop_front();
    return true;
}

bool AudioBuffer::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunks_.empty();
}

size_t AudioBuffer::chunks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunks_.size();
}

int64_t AudioBuffer::durationMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return duration_ms_;
}

int64_t AudioBuffer::frontPtsMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunks_.empty() ? end_pts_ms_ : chunks_.front().pts;
}

int64_t AudioBuffer::frontEndPtsMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return chunks_.empty() ? end_pts_ms_
                           : chunks_.front().pts + chunkDurationMs(chunks_.front());
}

int64_t AudioBuffer::endPtsMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return end_pts_ms_;
}

uint64_t AudioBuffer::droppedChunks() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
}

void AudioBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    chunks_.clear();
    duration_ms_ = 0;
    end_pts_ms_ = 0;
    dropped_ = 0;
}

}  // namespace player::audio
