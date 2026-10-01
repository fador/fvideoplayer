#pragma once

#include "audio/opus/Mdct.hpp"
#include "audio/opus/OpusTables.hpp"
#include "audio/opus/RangeDecoder.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace player::audio::opus {

// CELT layer state and per-frame decoding (libopus CELTDecoder, CELT-only
// mode). Handles the frame header, band energies, PVQ content, synthesis and
// the pitch post-filter, keeping the inter-frame history.
class CeltFrameDecoder {
public:
    void init(int channels);
    void reset();

    int channels() const { return channels_; }

    // Decodes one CELT frame (a single Opus frame, no TOC) into interleaved
    // PCM at 48 kHz: frame_samples * channels floats. lm is 0..3 and
    // stream_channels 1 or 2.
    void decode(RangeDecoder& decoder, int lm, int stream_channels, float* pcm);

private:
    static constexpr int kDecodeBufferSize = 2048;

    void synthesis(const float* x, int start, int end, int stream_channels, int is_transient, int lm,
                   int silence, float* const out_syn[2], float* freq);

    int channels_ = 0;
    std::array<float, 2 * kCeltBands> old_band_e_ = {};
    std::array<float, 2 * kCeltBands> old_log_e_ = {};
    std::array<float, 2 * kCeltBands> old_log_e2_ = {};
    std::array<float, 2 * kCeltBands> background_log_e_ = {};
    int postfilter_period_ = 0;
    int postfilter_period_old_ = 0;
    float postfilter_gain_ = 0;
    float postfilter_gain_old_ = 0;
    int postfilter_tapset_ = 0;
    int postfilter_tapset_old_ = 0;
    float preemph_mem_[2] = {};
    uint32_t rng_ = 0;
    std::array<std::array<float, kDecodeBufferSize + kCeltOverlap>, 2> decode_mem_ = {};
    MdctLookup mdct_;
    std::vector<float> scratch_;
};

}  // namespace player::audio::opus
