#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace player::playback {

// Monotonic playback clock with pause/resume and seek support. Thread-safe:
// the UI thread starts/pauses/seeks it, the pipeline thread reads the position
// to schedule audio against the video clock.
class MediaClock {
public:
    double position() const {
        if (!running_.load(std::memory_order_acquire)) {
            return base_seconds_.load(std::memory_order_relaxed);
        }
        const int64_t anchor = anchor_ns_.load(std::memory_order_relaxed);
        const double elapsed = static_cast<double>(nowNs() - anchor) * 1e-9;
        return base_seconds_.load(std::memory_order_relaxed) + elapsed;
    }

    void start(double position_seconds) {
        base_seconds_.store(position_seconds, std::memory_order_relaxed);
        anchor_ns_.store(nowNs(), std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
    }

    void pause() {
        if (running_.load(std::memory_order_acquire)) {
            const double current = position();
            base_seconds_.store(current, std::memory_order_relaxed);
            running_.store(false, std::memory_order_release);
        }
    }

    void setPosition(double position_seconds) {
        base_seconds_.store(position_seconds, std::memory_order_relaxed);
        anchor_ns_.store(nowNs(), std::memory_order_relaxed);
    }

    bool running() const { return running_.load(std::memory_order_acquire); }

private:
    static int64_t nowNs() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    std::atomic<bool> running_{false};
    std::atomic<double> base_seconds_{0.0};
    std::atomic<int64_t> anchor_ns_{0};
};

}  // namespace player::playback
