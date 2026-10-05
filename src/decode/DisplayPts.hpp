#pragma once

#include <cstdint>
#include <functional>
#include <queue>
#include <vector>

namespace player::decode {

// Maps input sample timestamps to decoded pictures for codecs whose decoder
// does not forward timestamps (H.264). The demuxer's samples arrive in decode
// order while pictures come out in display order, so each output picture takes
// the smallest pushed timestamp that has not been handed out yet. A picture
// without a remaining timestamp reuses the last one.
class DisplayPts {
public:
    void reset() {
        pending_ = {};
        last_ = 0;
    }

    void push(int64_t pts) { pending_.push(pts); }

    int64_t next() {
        if (!pending_.empty()) {
            last_ = pending_.top();
            pending_.pop();
        }
        return last_;
    }

private:
    std::priority_queue<int64_t, std::vector<int64_t>, std::greater<int64_t>> pending_;
    int64_t last_ = 0;
};

}  // namespace player::decode
