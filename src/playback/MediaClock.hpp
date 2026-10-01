#pragma once

#include <chrono>

namespace player::playback {

// Monotonic playback clock with pause/resume and seek support.
class MediaClock {
public:
    double position() const {
        if (!running_) {
            return base_seconds_;
        }
        const std::chrono::duration<double> elapsed =
            std::chrono::steady_clock::now() - anchor_;
        return base_seconds_ + elapsed.count();
    }

    void start(double position_seconds) {
        base_seconds_ = position_seconds;
        anchor_ = std::chrono::steady_clock::now();
        running_ = true;
    }

    void pause() {
        if (running_) {
            base_seconds_ = position();
            running_ = false;
        }
    }

    void setPosition(double position_seconds) {
        base_seconds_ = position_seconds;
        anchor_ = std::chrono::steady_clock::now();
    }

    bool running() const { return running_; }

private:
    bool running_ = false;
    double base_seconds_ = 0.0;
    std::chrono::steady_clock::time_point anchor_ = std::chrono::steady_clock::now();
};

}  // namespace player::playback
