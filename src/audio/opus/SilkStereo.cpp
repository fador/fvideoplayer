#include "audio/opus/SilkStereo.hpp"

#include "audio/opus/SilkMath.hpp"
#include "audio/opus/SilkTables.hpp"

namespace player::audio::silk {
namespace {

constexpr int kStereoQuantSubSteps = 5;
constexpr int kStereoInterpLenMs = 8;
constexpr int32_t kHalfStepQ16 = 6554;  // SILK_FIX_CONST(0.5 / 5, 16)

}  // namespace

void decodeStereoPred(opus::RangeDecoder& decoder, int32_t* pred_q13) {
    int ix[2][3];

    // Entropy decoding: joint index then the two sub-steps per channel.
    const int n = decoder.decodeIcdf(kSilkStereoPredJointIcdf, 8);
    ix[0][2] = n / 5;
    ix[1][2] = n - 5 * ix[0][2];
    for (int i = 0; i < 2; ++i) {
        ix[i][0] = decoder.decodeIcdf(kSilkUniform3Icdf, 8);
        ix[i][1] = decoder.decodeIcdf(kSilkUniform5Icdf, 8);
    }

    // Dequantize.
    for (int i = 0; i < 2; ++i) {
        ix[i][0] += 3 * ix[i][2];
        const int32_t low_q13 = kSilkStereoPredQuantQ13[ix[i][0]];
        const int32_t step_q13 =
            smulwb(kSilkStereoPredQuantQ13[ix[i][0] + 1] - low_q13, kHalfStepQ16);
        pred_q13[i] = low_q13 + static_cast<int32_t>(static_cast<int64_t>(step_q13) *
                                                     (2 * ix[i][1] + 1));
    }

    // The second predictor is subtracted to simplify application.
    pred_q13[0] -= pred_q13[1];
}

int decodeMidOnly(opus::RangeDecoder& decoder) {
    return decoder.decodeIcdf(kSilkStereoOnlyMidIcdf, 8);
}

void stereoMsToLr(SilkStereoState& state, int16_t* x1, int16_t* x2, const int32_t* pred_q13,
                  int fs_khz, int frame_length) {
    // Two samples of inter-frame state on each side.
    x1[0] = state.s_mid[0];
    x1[1] = state.s_mid[1];
    x2[0] = state.s_side[0];
    x2[1] = state.s_side[1];
    state.s_mid[0] = x1[frame_length];
    state.s_mid[1] = x1[frame_length + 1];
    state.s_side[0] = x2[frame_length];
    state.s_side[1] = x2[frame_length + 1];

    // Interpolate the predictors and add the prediction to the side channel.
    int32_t pred0_q13 = state.pred_prev_q13[0];
    int32_t pred1_q13 = state.pred_prev_q13[1];
    const int32_t denom_q16 = (1 << 16) / (kStereoInterpLenMs * fs_khz);
    const int32_t delta0_q13 =
        rshiftRound(smulbb(pred_q13[0] - state.pred_prev_q13[0], denom_q16), 16);
    const int32_t delta1_q13 =
        rshiftRound(smulbb(pred_q13[1] - state.pred_prev_q13[1], denom_q16), 16);

    const int interp = kStereoInterpLenMs * fs_khz;
    for (int n = 0; n < interp; ++n) {
        pred0_q13 += delta0_q13;
        pred1_q13 += delta1_q13;
        int32_t sum = ((x1[n] + x1[n + 2]) + (x1[n + 1] << 1)) << 9;
        sum = smlawb(x2[n + 1] << 8, sum, pred0_q13);
        sum = smlawb(sum, x1[n + 1] << 11, pred1_q13);
        x2[n + 1] = static_cast<int16_t>(sat16(rshiftRound(sum, 8)));
    }
    pred0_q13 = pred_q13[0];
    pred1_q13 = pred_q13[1];
    for (int n = interp; n < frame_length; ++n) {
        int32_t sum = ((x1[n] + x1[n + 2]) + (x1[n + 1] << 1)) << 9;
        sum = smlawb(x2[n + 1] << 8, sum, pred0_q13);
        sum = smlawb(sum, x1[n + 1] << 11, pred1_q13);
        x2[n + 1] = static_cast<int16_t>(sat16(rshiftRound(sum, 8)));
    }
    state.pred_prev_q13[0] = pred_q13[0];
    state.pred_prev_q13[1] = pred_q13[1];

    // Mid/side to left/right.
    for (int n = 0; n < frame_length; ++n) {
        const int32_t sum = x1[n + 1] + static_cast<int32_t>(x2[n + 1]);
        const int32_t diff = x1[n + 1] - static_cast<int32_t>(x2[n + 1]);
        x1[n + 1] = static_cast<int16_t>(sat16(sum));
        x2[n + 1] = static_cast<int16_t>(sat16(diff));
    }
}

}  // namespace player::audio::silk
