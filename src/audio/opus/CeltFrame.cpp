#include "audio/opus/CeltFrame.hpp"

#include "audio/opus/CeltAlloc.hpp"
#include "audio/opus/CeltBands.hpp"
#include "audio/opus/CeltEnergy.hpp"
#include "audio/opus/CeltFilter.hpp"
#include "audio/opus/CeltPost.hpp"

#include <algorithm>
#include <cmath>

namespace player::audio::opus {
namespace {

constexpr float kSilenceEnergy = -28.0f;
constexpr int kMdctSize = 2 * kCeltShortMdct << kCeltMaxLm;

// Decodes the per-band time/frequency resolution (celt_decoder.c tf_decode).
void tfDecode(int start, int end, int is_transient, int* tf_res, int lm, RangeDecoder& decoder) {
    const int budget = static_cast<int>(decoder.size()) * 8;
    int tell = decoder.tell();
    int logp = is_transient ? 2 : 4;
    const int tf_select_rsv = lm > 0 && tell + logp + 1 <= budget ? 1 : 0;
    const int budget_left = budget - tf_select_rsv;

    int tf_changed = 0;
    int curr = 0;
    for (int i = start; i < end; ++i) {
        if (tell + logp <= budget_left) {
            curr ^= decoder.decodeBitLogp(logp) ? 1 : 0;
            tell = decoder.tell();
            tf_changed |= curr;
        }
        tf_res[i] = curr;
        logp = is_transient ? 4 : 5;
    }

    int tf_select = 0;
    if (tf_select_rsv != 0 && kCeltTfSelect[lm][4 * is_transient + 0 + tf_changed] !=
                                 kCeltTfSelect[lm][4 * is_transient + 2 + tf_changed]) {
        tf_select = decoder.decodeBitLogp(1) ? 1 : 0;
    }
    for (int i = start; i < end; ++i) {
        tf_res[i] = kCeltTfSelect[lm][4 * is_transient + 2 * tf_select + tf_res[i]];
    }
}

}  // namespace

void CeltFrameDecoder::init(int channels) {
    channels_ = std::min(channels, 2);
    mdct_.init(kMdctSize, kCeltMaxLm);
    scratch_.resize(3 * kCeltFrame48k);
    reset();
}

void CeltFrameDecoder::reset() {
    old_band_e_.fill(0.0f);
    old_log_e_.fill(kSilenceEnergy);
    old_log_e2_.fill(kSilenceEnergy);
    background_log_e_.fill(0.0f);
    postfilter_period_ = 0;
    postfilter_period_old_ = 0;
    postfilter_gain_ = 0;
    postfilter_gain_old_ = 0;
    postfilter_tapset_ = 0;
    postfilter_tapset_old_ = 0;
    preemph_mem_[0] = 0;
    preemph_mem_[1] = 0;
    rng_ = 0;
    for (auto& mem : decode_mem_) {
        mem.fill(0.0f);
    }
}

void CeltFrameDecoder::synthesis(const float* x, int start, int end, int stream_channels,
                                 int is_transient, int lm, int silence, float* const out_syn[2],
                                 float* freq) {
    const int m = 1 << lm;
    const int n = kCeltShortMdct << lm;
    int blocks;
    int nb;
    int shift;
    if (is_transient) {
        blocks = m;
        nb = kCeltShortMdct;
        shift = kCeltMaxLm;
    } else {
        blocks = 1;
        nb = kCeltShortMdct << lm;
        shift = kCeltMaxLm - lm;
    }

    for (int c = 0; c < stream_channels; ++c) {
        denormaliseBands(x + c * n, old_band_e_.data() + c * kCeltBands, start, end, m, 1, silence,
                         freq);
        for (int b = 0; b < blocks; ++b) {
            mdct_.backward(freq + b, out_syn[c] + nb * b, kCeltWindow, kCeltOverlap, shift, blocks);
        }
    }
}

void CeltFrameDecoder::decode(RangeDecoder& decoder, int lm, int stream_channels, float* pcm) {
    const int m = 1 << lm;
    const int n = m * kCeltShortMdct;
    const int start = 0;
    const int end = kCeltBands;
    const int len = static_cast<int>(decoder.size());

    float* out_syn[2];
    out_syn[0] = decode_mem_[0].data() + kDecodeBufferSize - n;
    out_syn[1] = decode_mem_[1].data() + kDecodeBufferSize - n;

    // A mono frame reuses the louder of the previous stereo energies.
    if (stream_channels == 1) {
        for (int i = 0; i < kCeltBands; ++i) {
            old_band_e_[i] = std::max(old_band_e_[i], old_band_e_[kCeltBands + i]);
        }
    }

    const int total_bits = len * 8;
    int tell = decoder.tell();
    int silence;
    if (tell >= total_bits) {
        silence = 1;
    } else if (tell == 1) {
        silence = decoder.decodeBitLogp(15) ? 1 : 0;
    } else {
        silence = 0;
    }
    if (silence) {
        decoder.skipToEnd(total_bits);
        tell = total_bits;
    }

    // Pitch post-filter parameters.
    float postfilter_gain = 0;
    int postfilter_pitch = 0;
    int postfilter_tapset = 0;
    if (start == 0 && tell + 16 <= total_bits) {
        if (decoder.decodeBitLogp(1)) {
            const int octave = static_cast<int>(decoder.decodeUint(6));
            postfilter_pitch = (16 << octave) + static_cast<int>(decoder.decodeBits(4 + octave)) - 1;
            const int qg = static_cast<int>(decoder.decodeBits(3));
            if (decoder.tell() + 2 <= total_bits) {
                postfilter_tapset = decoder.decodeIcdf(kCeltTapsetIcdf, 2);
            }
            postfilter_gain = 0.09375f * (qg + 1);
        }
        tell = decoder.tell();
    }

    int is_transient = 0;
    if (lm > 0 && tell + 3 <= total_bits) {
        is_transient = decoder.decodeBitLogp(3) ? 1 : 0;
        tell = decoder.tell();
    }
    const int short_blocks = is_transient ? m : 0;

    const int intra_ener = tell + 3 <= total_bits && decoder.decodeBitLogp(3) ? 1 : 0;
    decodeCoarseEnergy(decoder, start, end, old_band_e_.data(), intra_ener != 0, stream_channels,
                       lm);

    int tf_res[kCeltBands];
    tfDecode(start, end, is_transient, tf_res, lm, decoder);

    tell = decoder.tell();
    int spread = static_cast<int>(Spread::Normal);
    if (tell + 4 <= total_bits) {
        spread = decoder.decodeIcdf(kCeltSpreadIcdf, 5);
    }

    int cap[kCeltBands];
    initCaps(cap, lm, stream_channels);

    // Dynamic allocation boosts.
    int offsets[kCeltBands] = {};
    int dynalloc_logp = 6;
    int32_t total_bits_frac = static_cast<int32_t>(total_bits) << kCeltBitsRes;
    int tell_frac = decoder.tellFrac();
    for (int i = start; i < end; ++i) {
        const int width = stream_channels * (kCeltEBands[i + 1] - kCeltEBands[i]) << lm;
        const int quanta =
            std::min(width << kCeltBitsRes, std::max(6 << kCeltBitsRes, width));
        int dynalloc_loop_logp = dynalloc_logp;
        int boost = 0;
        while (tell_frac + (dynalloc_loop_logp << kCeltBitsRes) < total_bits_frac &&
               boost < cap[i]) {
            const int flag = decoder.decodeBitLogp(dynalloc_loop_logp) ? 1 : 0;
            tell_frac = decoder.tellFrac();
            if (!flag) {
                break;
            }
            boost += quanta;
            total_bits_frac -= quanta;
            dynalloc_loop_logp = 1;
        }
        offsets[i] = boost;
        if (boost > 0) {
            dynalloc_logp = std::max(2, dynalloc_logp - 1);
        }
    }

    const int alloc_trim =
        tell_frac + (6 << kCeltBitsRes) <= total_bits_frac ? decoder.decodeIcdf(kCeltTrimIcdf, 7) : 5;

    int32_t bits = (static_cast<int32_t>(len) * 8 << kCeltBitsRes) - decoder.tellFrac() - 1;
    const int anti_collapse_rsv =
        is_transient && lm >= 2 && bits >= ((lm + 2) << kCeltBitsRes) ? (1 << kCeltBitsRes) : 0;
    bits -= anti_collapse_rsv;

    Allocation allocation;
    computeAllocation(decoder, start, end, offsets, cap, alloc_trim, bits, stream_channels, lm,
                      allocation);

    decodeFineEnergy(decoder, start, end, old_band_e_.data(), allocation.fine_quant,
                     stream_channels);

    // Shift the previous output down to make room for this frame.
    for (int c = 0; c < channels_; ++c) {
        auto& mem = decode_mem_[c];
        std::copy(mem.begin() + n, mem.begin() + kDecodeBufferSize + kCeltOverlap / 2, mem.begin());
    }

    float* x = scratch_.data();
    float* freq = x + 2 * kCeltFrame48k;
    uint8_t collapse_masks[2 * kCeltBands] = {};

    BandsParams params;
    params.short_blocks = short_blocks;
    params.spread = static_cast<Spread>(spread);
    params.dual_stereo = allocation.dual_stereo;
    params.intensity = allocation.intensity;
    params.tf_res = tf_res;
    params.pulses = allocation.pulses;
    params.total_bits = static_cast<int32_t>(len) * (8 << kCeltBitsRes) - anti_collapse_rsv;
    params.balance = allocation.balance;
    params.lm = lm;
    params.coded_bands = allocation.coded_bands;
    params.disable_inv = channels_ == 1 ? 1 : 0;
    uint32_t seed = rng_;
    quantAllBands(decoder, params, stream_channels, x, stream_channels == 2 ? x + n : nullptr,
                  collapse_masks, seed);
    rng_ = seed;

    int anti_collapse_on = 0;
    if (anti_collapse_rsv > 0) {
        anti_collapse_on = static_cast<int>(decoder.decodeBits(1));
    }

    decodeFinalEnergy(decoder, start, end, old_band_e_.data(), allocation.fine_quant,
                      allocation.fine_priority, len * 8 - decoder.tell(), stream_channels);

    if (anti_collapse_on) {
        // The reference passes the seed by value; the state is not advanced.
        uint32_t ac_seed = rng_;
        antiCollapse(x, collapse_masks, lm, stream_channels, n, start, end, old_band_e_.data(),
                     old_log_e_.data(), old_log_e2_.data(), allocation.pulses, ac_seed);
    }

    if (silence) {
        for (int i = 0; i < stream_channels * kCeltBands; ++i) {
            old_band_e_[i] = kSilenceEnergy;
        }
    }

    synthesis(x, start, end, stream_channels, is_transient, lm, silence, out_syn, freq);

    for (int c = 0; c < stream_channels; ++c) {
        postfilter_period_ = std::max(postfilter_period_, kCombFilterMinPeriod);
        postfilter_period_old_ = std::max(postfilter_period_old_, kCombFilterMinPeriod);
        combFilter(out_syn[c], out_syn[c], postfilter_period_old_, postfilter_period_,
                   kCeltShortMdct, postfilter_gain_old_, postfilter_gain_, postfilter_tapset_old_,
                   postfilter_tapset_, kCeltWindow, kCeltOverlap);
        if (lm != 0) {
            combFilter(out_syn[c] + kCeltShortMdct, out_syn[c] + kCeltShortMdct, postfilter_period_,
                       postfilter_pitch, n - kCeltShortMdct, postfilter_gain_, postfilter_gain,
                       postfilter_tapset_, postfilter_tapset, kCeltWindow, kCeltOverlap);
        }
    }
    postfilter_period_old_ = postfilter_period_;
    postfilter_gain_old_ = postfilter_gain_;
    postfilter_tapset_old_ = postfilter_tapset_;
    postfilter_period_ = postfilter_pitch;
    postfilter_gain_ = postfilter_gain;
    postfilter_tapset_ = postfilter_tapset;
    if (lm != 0) {
        postfilter_period_old_ = postfilter_period_;
        postfilter_gain_old_ = postfilter_gain_;
        postfilter_tapset_old_ = postfilter_tapset_;
    }

    if (stream_channels == 1) {
        std::copy(old_band_e_.begin(), old_band_e_.begin() + kCeltBands,
                  old_band_e_.begin() + kCeltBands);
    }

    if (!is_transient) {
        std::copy(old_log_e_.begin(), old_log_e_.end(), old_log_e2_.begin());
        std::copy(old_band_e_.begin(), old_band_e_.end(), old_log_e_.begin());
        const float max_inc = m * 0.001f;
        for (int i = 0; i < 2 * kCeltBands; ++i) {
            background_log_e_[i] = std::min(background_log_e_[i] + max_inc, old_band_e_[i]);
        }
    } else {
        for (int i = 0; i < 2 * kCeltBands; ++i) {
            old_log_e_[i] = std::min(old_log_e_[i], old_band_e_[i]);
        }
    }

    // Unused bands are silenced for the next frame's history.
    for (int c = 0; c < 2; ++c) {
        for (int i = 0; i < start; ++i) {
            old_band_e_[c * kCeltBands + i] = 0;
            old_log_e_[c * kCeltBands + i] = kSilenceEnergy;
            old_log_e2_[c * kCeltBands + i] = kSilenceEnergy;
        }
        for (int i = end; i < kCeltBands; ++i) {
            old_band_e_[c * kCeltBands + i] = 0;
            old_log_e_[c * kCeltBands + i] = kSilenceEnergy;
            old_log_e2_[c * kCeltBands + i] = kSilenceEnergy;
        }
    }

    rng_ = decoder.rng();

    deemphasis(out_syn, pcm, n, stream_channels, 1, kCeltPreemph48k, preemph_mem_);
}

}  // namespace player::audio::opus
