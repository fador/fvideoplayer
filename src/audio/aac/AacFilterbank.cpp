#include "audio/aac/AacFilterbank.hpp"

#include "audio/aac/Window.hpp"

#include <algorithm>
#include <vector>

namespace player::audio::aac {
namespace {

// Normalization to the standard +/-1 float range: the spec's inverse
// filterbank uses 2/window_length (2/2048 for a long block) and the fixed
// point reference carries Q15 spectral data, so the 2/N used by our MDCT
// (N = bin count) is 2^16 times the normalized output scale.
constexpr float kSynthesisScale = 1.0f / 65536.0f;

constexpr unsigned kLongSize = 2 * kFrameSamples;
constexpr unsigned kShortSize = 2 * kWindowSize;
constexpr unsigned kFlat = 448;

//    AAC window sequences (window_shape 0 = sine, 1 = KBD):
//
//    ONLY_LONG    |<--------- long 2048 --------->|
//    LONG_START   |<-- long 1024 -->| 1.0 (448) |<short 128>| 0 (448) |
//    EIGHT_SHORT  0 (448) | 8 x short 256 at 128 strides | 0 (448)
//    LONG_STOP    0 (448) | <short 128> | 1.0 (448) |<-- long 1024 -->|
//
//    Shapes: the overlap with the previous frame uses the previous frame's
//    shape, overlaps inside the frame use the current shape. That keeps the
//    Princen-Bradley condition when the signaled window_shape changes.
struct WindowSet {
    float long_window[2][kLongSize];
    float short_window[2][kShortSize];

    WindowSet() {
        for (unsigned shape = 0; shape < 2; ++shape) {
            const std::vector<float> long_values =
                shape == 0 ? sineWindow(kFrameSamples) : kbdWindow(kFrameSamples, 4.0f);
            const std::vector<float> short_values =
                shape == 0 ? sineWindow(kWindowSize) : kbdWindow(kWindowSize, 6.0f);
            std::copy(long_values.begin(), long_values.end(), long_window[shape]);
            std::copy(short_values.begin(), short_values.end(), short_window[shape]);
        }
    }
};

const WindowSet& windowSet() {
    static const WindowSet windows;
    return windows;
}

}  // namespace

void Filterbank::reset() {
    std::fill(std::begin(overlap_), std::end(overlap_), 0.0f);
    previous_shape_ = false;
}

void Filterbank::synthesize(const float* spectrum, const IcsInfo& info, float* out) {
    const WindowSet& windows = windowSet();
    const unsigned current_shape = info.kbd_window ? 1 : 0;
    const unsigned previous_shape = previous_shape_ ? 1 : 0;
    const float* long_previous = windows.long_window[previous_shape];
    const float* long_current = windows.long_window[current_shape];
    const float* short_previous = windows.short_window[previous_shape];
    const float* short_current = windows.short_window[current_shape];

    if (info.sequence == WindowSequence::EightShort) {
        std::fill(std::begin(block_), std::end(block_), 0.0f);
        for (unsigned w = 0; w < 8; ++w) {
            short_mdct_.inverse(spectrum + w * kWindowSize, short_block_);
            float* destination = block_ + kFlat + w * kWindowSize;
            for (unsigned i = 0; i < kShortSize; ++i) {
                // Only the first short window straddles the frame boundary.
                const float* window =
                    (w == 0 && i < kWindowSize) ? short_previous : short_current;
                destination[i] += short_block_[i] * window[i] * kSynthesisScale;
            }
        }
    } else {
        std::copy(long_previous, long_previous + kFrameSamples, window_);
        if (info.sequence == WindowSequence::OnlyLong) {
            std::copy(long_current + kFrameSamples, long_current + kLongSize,
                      window_ + kFrameSamples);
        } else if (info.sequence == WindowSequence::LongStart) {
            std::fill(window_ + kFrameSamples, window_ + kFrameSamples + kFlat, 1.0f);
            std::copy(short_current + kWindowSize, short_current + kShortSize,
                      window_ + kFrameSamples + kFlat);
            std::fill(window_ + kFrameSamples + kFlat + kWindowSize, window_ + kLongSize, 0.0f);
        } else {  // LongStop
            std::fill(window_, window_ + kFlat, 0.0f);
            std::copy(short_previous, short_previous + kWindowSize, window_ + kFlat);
            std::fill(window_ + kFlat + kWindowSize, window_ + kFrameSamples, 1.0f);
            std::copy(long_current + kFrameSamples, long_current + kLongSize,
                      window_ + kFrameSamples);
        }
        long_mdct_.inverse(spectrum, block_);
        for (unsigned i = 0; i < kLongSize; ++i) {
            block_[i] *= window_[i] * kSynthesisScale;
        }
    }
    previous_shape_ = info.kbd_window;

    for (unsigned i = 0; i < kFrameSamples; ++i) {
        out[i] = overlap_[i] + block_[i];
        overlap_[i] = block_[i + kFrameSamples];
    }
}

}  // namespace player::audio::aac
