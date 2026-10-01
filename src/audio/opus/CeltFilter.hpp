#pragma once

namespace player::audio::opus {

// Pitch post-filter bounds (RFC 6716 section 4.3.7).
constexpr int kCombFilterMinPeriod = 15;
constexpr int kCombFilterMaxPeriod = 1024;

// Pitch post-filter with a crossfade from the previous frame's parameters.
// y and x may alias; x must have kCombFilterMaxPeriod samples of history
// before index 0. tapset selects a preset gain triplet.
void combFilter(float* y, float* x, int t0, int t1, int n, float g0, float g1, int tapset0,
                int tapset1, const float* window, int overlap);

// Removes the pre-emphasis filter (1 - coef0 * z^-1). pcm is interleaved and
// holds n/downsample samples per channel; mem keeps one sample of state per
// channel.
void deemphasis(float* const in[], float* pcm, int n, int channels, int downsample, float coef0,
                float* mem);

}  // namespace player::audio::opus
