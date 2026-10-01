#include "audio/opus/SilkGain.hpp"

#include "audio/opus/SilkMath.hpp"

#include <algorithm>

namespace player::audio::silk {
namespace {

constexpr int kMinQGainDb = 2;
constexpr int kMaxQGainDb = 88;
constexpr int kNLevelsQGain = 64;
constexpr int kMinDeltaGainQuant = -4;
constexpr int kMaxDeltaGainQuant = 36;

// SILK_FIX_CONST-style integer constants from gain_quant.c.
constexpr int32_t kInvScaleQ16 =
    (65536 * (((kMaxQGainDb - kMinQGainDb) * 128) / 6)) / (kNLevelsQGain - 1);
constexpr int32_t kGainOffset = ((kMinQGainDb * 128) / 6 + 16 * 128);

}  // namespace

int32_t log2lin(int32_t in_log_q7) {
    if (in_log_q7 < 0) {
        return 0;
    }
    if (in_log_q7 >= 3967) {
        return kInt32Max;
    }

    int32_t out = 1 << (in_log_q7 >> 7);
    const int32_t frac_q7 = in_log_q7 & 0x7F;
    if (in_log_q7 < 2048) {
        out = out + ((out * smlawb(frac_q7, frac_q7 * (128 - frac_q7), -174)) >> 7);
    } else {
        out = out + ((out >> 7) * smlawb(frac_q7, frac_q7 * (128 - frac_q7), -174));
    }
    return out;
}

void gainsDequant(int32_t* gain_q16, const int8_t* ind, int8_t& prev_ind, int conditional,
                  int nb_subfr) {
    for (int k = 0; k < nb_subfr; ++k) {
        if (k == 0 && conditional == 0) {
            // The gain may not drop more than 16 steps (about 21.8 dB).
            prev_ind = static_cast<int8_t>(std::max<int>(ind[k], prev_ind - 16));
        } else {
            const int ind_tmp = ind[k] + kMinDeltaGainQuant;
            const int double_step = 2 * kMaxDeltaGainQuant - kNLevelsQGain + prev_ind;
            if (ind_tmp > double_step) {
                prev_ind = static_cast<int8_t>(prev_ind + (ind_tmp << 1) - double_step);
            } else {
                prev_ind = static_cast<int8_t>(prev_ind + ind_tmp);
            }
        }
        prev_ind = static_cast<int8_t>(limit(prev_ind, 0, kNLevelsQGain - 1));

        gain_q16[k] = log2lin(std::min(smulwb(kInvScaleQ16, prev_ind) + kGainOffset, 3967));
    }
}

}  // namespace player::audio::silk
