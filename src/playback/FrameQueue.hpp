#pragma once

#include "decode/KvzdecDecoder.hpp"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>

namespace player::playback {

// Bounded decoded-frame queue. The pipeline thread pushes (blocking while
// full), the UI thread pops due frames. This is the decode-ahead buffer.
class FrameQueue {
public:
    explicit FrameQueue(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {}

    void setCapacity(size_t capacity) {
        std::lock_guard<std::mutex> lock(mutex_);
        capacity_ = capacity == 0 ? 1 : capacity;
        not_full_.notify_all();
    }

    // Blocks until there is room, the queue is aborted, or `cancelled` returns
    // true. Returns false when the frame was not enqueued.
    template <typename CancelFn>
    bool push(decode::DecodedFrame&& frame, CancelFn cancelled) {
        std::unique_lock<std::mutex> lock(mutex_);
        not_full_.wait(lock, [&] { return aborted_ || cancelled() || queue_.size() < capacity_; });
        if (aborted_ || cancelled()) {
            return false;
        }
        queue_.push_back(std::move(frame));
        not_empty_.notify_one();
        return true;
    }

    // Pops the front frame if its pts is at or before due_ms.
    bool tryPopDue(int64_t due_ms, decode::DecodedFrame& out) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.empty() || queue_.front().pts > due_ms) {
            return false;
        }
        out = std::move(queue_.front());
        queue_.pop_front();
        not_full_.notify_one();
        return true;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.clear();
        not_full_.notify_all();
    }

    void abort() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
        not_full_.notify_all();
        not_empty_.notify_all();
    }

    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = false;
        queue_.clear();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

    size_t capacity() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return capacity_;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable not_full_;
    std::condition_variable not_empty_;
    std::deque<decode::DecodedFrame> queue_;
    size_t capacity_;
    bool aborted_ = false;
};

}  // namespace player::playback
