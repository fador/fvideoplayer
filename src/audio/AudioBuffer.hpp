#pragma once

#include "audio/Pcm.hpp"

#include <cstdint>
#include <deque>
#include <mutex>

namespace player::audio {

// Thread-safe FIFO of decoded PCM chunks with media timestamps in
// milliseconds. It decouples decoding from playback: the pipeline fills it
// ahead of the media clock and drains it into the sink. When the capacity is
// exceeded the oldest chunk is dropped so latency stays bounded.
class AudioBuffer {
public:
    explicit AudioBuffer(int64_t capacity_ms);

    void push(PcmBuffer&& chunk);
    bool pop(PcmBuffer& out);

    bool empty() const;
    size_t chunks() const;
    int64_t durationMs() const;
    // Presentation time of the oldest / newest chunk.
    int64_t frontPtsMs() const;
    int64_t frontEndPtsMs() const;
    int64_t endPtsMs() const;
    uint64_t droppedChunks() const;
    void clear();

private:
    int64_t chunkDurationMs(const PcmBuffer& chunk) const;

    mutable std::mutex mutex_;
    std::deque<PcmBuffer> chunks_;
    int64_t capacity_ms_ = 0;
    int64_t duration_ms_ = 0;
    int64_t end_pts_ms_ = 0;
    uint64_t dropped_ = 0;
};

}  // namespace player::audio
