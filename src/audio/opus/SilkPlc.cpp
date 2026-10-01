#include "audio/opus/SilkDecoder.hpp"

#include "audio/opus/SilkLpc.hpp"
#include "audio/opus/SilkMath.hpp"

#include <algorithm>
#include <cstring>

namespace player::audio::silk {
namespace {

// PLC.c constants.
constexpr int32_t kHarmAttQ15[2] = {32440, 31130};               // 0.99, 0.95
constexpr int32_t kPlcRandAttenuateVQ15[2] = {31130, 26214};     // 0.95, 0.8
constexpr int32_t kPlcRandAttenuateUvQ15[2] = {32440, 29491};    // 0.99, 0.9
constexpr int32_t kVpitchGainStartMinQ14 = 11469;                // 0.7
constexpr int32_t kVpitchGainStartMaxQ14 = 15565;                // 0.95
constexpr int32_t kBweCoefQ16 = 64881;                           // 0.99
constexpr int32_t kLog2InvLpcGainHighThres = 3;
constexpr int32_t kLog2InvLpcGainLowThres = 8;
constexpr int32_t kPitchDriftFacQ16 = 655;
constexpr int32_t kMaxPitchLagMs = 18;
constexpr int kRandBufSize = 128;
constexpr int kRandBufMask = 127;

// CNG.c constants.
constexpr int kCngBufMaskMax = 255;
constexpr int32_t kCngGainSmthQ16 = 4634;
constexpr int32_t kCngNlsfSmthQ16 = 16348;
constexpr int32_t kCngRandSeedInit = 3176576;

// Energy of a vector of int16 samples, shifted to fit in an int32
// (silk_sum_sqr_shift).
void sumSqrShift(int32_t* energy, int* shift, const int16_t* x, int len) {
    int shft = 31 - clz32(len);
    int32_t nrg = len;
    int i;
    for (i = 0; i < len - 1; i += 2) {
        uint32_t nrg_tmp = static_cast<uint32_t>(smulbb(x[i], x[i]));
        nrg_tmp = static_cast<uint32_t>(
            smlabbOvflw(static_cast<int32_t>(nrg_tmp), x[i + 1], x[i + 1]));
        nrg = static_cast<int32_t>(static_cast<uint32_t>(nrg) + (nrg_tmp >> shft));
    }
    if (i < len) {
        const uint32_t nrg_tmp = static_cast<uint32_t>(smulbb(x[i], x[i]));
        nrg = static_cast<int32_t>(static_cast<uint32_t>(nrg) + (nrg_tmp >> shft));
    }

    // Leave two bits of headroom in the result.
    shft = std::max(0, shft + 3 - clz32(nrg));
    nrg = 0;
    for (i = 0; i < len - 1; i += 2) {
        uint32_t nrg_tmp = static_cast<uint32_t>(smulbb(x[i], x[i]));
        nrg_tmp = static_cast<uint32_t>(
            smlabbOvflw(static_cast<int32_t>(nrg_tmp), x[i + 1], x[i + 1]));
        nrg = static_cast<int32_t>(static_cast<uint32_t>(nrg) + (nrg_tmp >> shft));
    }
    if (i < len) {
        const uint32_t nrg_tmp = static_cast<uint32_t>(smulbb(x[i], x[i]));
        nrg = static_cast<int32_t>(static_cast<uint32_t>(nrg) + (nrg_tmp >> shft));
    }

    *shift = shft;
    *energy = nrg;
}

// Energy of the last two subframes, used to pick the noise buffer
// (silk_PLC_energy).
void plcEnergy(int32_t* energy1, int* shift1, int32_t* energy2, int* shift2,
               const int32_t* exc_q14, const int32_t* prev_gain_q10, int subfr_length,
               int nb_subfr) {
    int16_t exc_buf[2 * 80];
    int16_t* ptr = exc_buf;
    for (int k = 0; k < 2; ++k) {
        for (int i = 0; i < subfr_length; ++i) {
            ptr[i] = static_cast<int16_t>(sat16(
                smulww(exc_q14[i + (k + nb_subfr - 2) * subfr_length], prev_gain_q10[k]) >> 8));
        }
        ptr += subfr_length;
    }
    sumSqrShift(energy1, shift1, exc_buf, subfr_length);
    sumSqrShift(energy2, shift2, exc_buf + subfr_length, subfr_length);
}

// CNG excitation: random samples from a buffer (silk_CNG_exc).
void cngExc(int32_t* exc_q14, const int32_t* exc_buf_q14, int length, int32_t* rand_seed) {
    int exc_mask = kCngBufMaskMax;
    while (exc_mask > length) {
        exc_mask >>= 1;
    }

    int32_t seed = *rand_seed;
    for (int i = 0; i < length; ++i) {
        seed = rand32(seed);
        const int idx = (seed >> 24) & exc_mask;
        exc_q14[i] = exc_buf_q14[idx];
    }
    *rand_seed = seed;
}

}  // namespace

void SilkDecoder::plc(ChannelState& channel, SilkDecoderControl& control, int16_t* frame,
                      Lost lost) {
    if (channel.fs_khz != channel.plc.fs_khz) {
        // silk_PLC_Reset
        channel.plc.pitch_l_q8 = channel.frame_length << 7;
        channel.plc.prev_gain_q16[0] = 65536;
        channel.plc.prev_gain_q16[1] = 65536;
        channel.plc.subfr_length = 20;
        channel.plc.nb_subfr = 2;
        channel.plc.fs_khz = channel.fs_khz;
    }

    if (lost == Lost::Yes) {
        plcConceal(channel, control, frame);
        channel.loss_cnt++;
    } else {
        plcUpdate(channel, control);
    }
}

void SilkDecoder::plcUpdate(ChannelState& channel, const SilkDecoderControl& control) {
    PlcState& plc_state = channel.plc;
    channel.prev_signal_type = channel.indices.signal_type;

    int32_t ltp_gain_q14 = 0;
    if (channel.indices.signal_type == kTypeVoiced) {
        // Find the last subframe that contains a pitch pulse.
        for (int j = 0; j * channel.subfr_length < control.pitch_l[channel.nb_subfr - 1]; ++j) {
            if (j == channel.nb_subfr) {
                break;
            }
            int32_t temp_ltp_gain_q14 = 0;
            for (int i = 0; i < kLtpOrder; ++i) {
                temp_ltp_gain_q14 +=
                    control.ltp_coef_q14[(channel.nb_subfr - 1 - j) * kLtpOrder + i];
            }
            if (temp_ltp_gain_q14 > ltp_gain_q14) {
                ltp_gain_q14 = temp_ltp_gain_q14;
                std::copy(control.ltp_coef_q14 + (channel.nb_subfr - 1 - j) * kLtpOrder,
                          control.ltp_coef_q14 + (channel.nb_subfr - 1 - j) * kLtpOrder + kLtpOrder,
                          plc_state.ltp_coef_q14);
                plc_state.pitch_l_q8 = control.pitch_l[channel.nb_subfr - 1 - j] << 8;
            }
        }

        std::fill(plc_state.ltp_coef_q14, plc_state.ltp_coef_q14 + kLtpOrder, static_cast<int16_t>(0));
        plc_state.ltp_coef_q14[kLtpOrder / 2] = static_cast<int16_t>(ltp_gain_q14);

        // Limit the LTP coefficients.
        if (ltp_gain_q14 < kVpitchGainStartMinQ14) {
            const int32_t tmp = kVpitchGainStartMinQ14 << 10;
            const int scale_q10 = div32(tmp, std::max(ltp_gain_q14, 1));
            for (int i = 0; i < kLtpOrder; ++i) {
                plc_state.ltp_coef_q14[i] =
                    static_cast<int16_t>(smulbb(plc_state.ltp_coef_q14[i], scale_q10) >> 10);
            }
        } else if (ltp_gain_q14 > kVpitchGainStartMaxQ14) {
            const int32_t tmp = kVpitchGainStartMaxQ14 << 14;
            const int scale_q14 = div32(tmp, std::max(ltp_gain_q14, 1));
            for (int i = 0; i < kLtpOrder; ++i) {
                plc_state.ltp_coef_q14[i] =
                    static_cast<int16_t>(smulbb(plc_state.ltp_coef_q14[i], scale_q14) >> 14);
            }
        }
    } else {
        plc_state.pitch_l_q8 = (channel.fs_khz * kMaxPitchLagMs) << 8;
        std::fill(plc_state.ltp_coef_q14, plc_state.ltp_coef_q14 + kLtpOrder, static_cast<int16_t>(0));
    }

    std::copy(control.pred_coef_q12[1], control.pred_coef_q12[1] + channel.lpc_order,
              plc_state.prev_lpc_q12);
    plc_state.prev_ltp_scale_q14 = control.ltp_scale_q14;
    plc_state.prev_gain_q16[0] = control.gains_q16[channel.nb_subfr - 2];
    plc_state.prev_gain_q16[1] = control.gains_q16[channel.nb_subfr - 1];
    plc_state.subfr_length = channel.subfr_length;
    plc_state.nb_subfr = channel.nb_subfr;
}

void SilkDecoder::plcConceal(ChannelState& channel, SilkDecoderControl& control, int16_t* frame) {
    PlcState& plc_state = channel.plc;
    const int lpc_order = channel.lpc_order;
    const int ltp_mem_length = channel.ltp_mem_length;

    std::vector<int16_t> s_ltp(ltp_mem_length);
    std::vector<int32_t> s_ltp_q14(ltp_mem_length + channel.frame_length);
    int16_t a_q12[kMaxLpcOrder];

    const int32_t prev_gain_q10[2] = {plc_state.prev_gain_q16[0] >> 6,
                                      plc_state.prev_gain_q16[1] >> 6};

    if (channel.first_frame_after_reset) {
        std::fill(plc_state.prev_lpc_q12, plc_state.prev_lpc_q12 + kMaxLpcOrder,
                  static_cast<int16_t>(0));
    }

    int32_t energy1;
    int shift1;
    int32_t energy2;
    int shift2;
    plcEnergy(&energy1, &shift1, &energy2, &shift2, channel.exc_q14, prev_gain_q10,
              channel.subfr_length, channel.nb_subfr);

    const int32_t* rand_ptr;
    if ((energy1 >> shift2) < (energy2 >> shift1)) {
        // First subframe has the lowest energy.
        rand_ptr = &channel.exc_q14[std::max(
            0, (plc_state.nb_subfr - 1) * plc_state.subfr_length - kRandBufSize)];
    } else {
        rand_ptr = &channel.exc_q14[std::max(
            0, plc_state.nb_subfr * plc_state.subfr_length - kRandBufSize)];
    }

    int16_t* b_q14 = plc_state.ltp_coef_q14;
    int32_t rand_scale_q14 = plc_state.rand_scale_q14;

    const int32_t harm_gain_q15 = kHarmAttQ15[std::min(1, channel.loss_cnt)];
    int32_t rand_gain_q15;
    if (channel.prev_signal_type == kTypeVoiced) {
        rand_gain_q15 = kPlcRandAttenuateVQ15[std::min(1, channel.loss_cnt)];
    } else {
        rand_gain_q15 = kPlcRandAttenuateUvQ15[std::min(1, channel.loss_cnt)];
    }

    // LPC concealment: apply BWE to the previous LPC.
    bwexpander(plc_state.prev_lpc_q12, lpc_order, kBweCoefQ16);
    std::copy(plc_state.prev_lpc_q12, plc_state.prev_lpc_q12 + lpc_order, a_q12);

    if (channel.loss_cnt == 0) {
        rand_scale_q14 = 1 << 14;

        if (channel.prev_signal_type == kTypeVoiced) {
            // Reduce the random noise gain for voiced frames.
            for (int i = 0; i < kLtpOrder; ++i) {
                rand_scale_q14 -= b_q14[i];
            }
            rand_scale_q14 = std::max(3277, rand_scale_q14);  // 0.2
            rand_scale_q14 = (smulbb(rand_scale_q14, plc_state.prev_ltp_scale_q14) >> 14);
        } else {
            // Reduce the random noise for unvoiced frames with high LPC gain.
            const int32_t inv_gain_q30 = lpcInversePredGain(plc_state.prev_lpc_q12, lpc_order);
            int32_t down_scale_q30 =
                std::min(static_cast<int32_t>(1 << 30) >> kLog2InvLpcGainHighThres, inv_gain_q30);
            down_scale_q30 =
                std::max(static_cast<int32_t>(1 << 30) >> kLog2InvLpcGainLowThres, down_scale_q30);
            down_scale_q30 <<= kLog2InvLpcGainHighThres;
            rand_gain_q15 = smulwb(down_scale_q30, rand_gain_q15) >> 14;
        }
    }

    int32_t rand_seed = plc_state.rand_seed;
    int lag = rshiftRound(plc_state.pitch_l_q8, 8);
    int s_ltp_buf_idx = ltp_mem_length;

    // Rewhiten the LTP state.
    const int idx = ltp_mem_length - lag - lpc_order - kLtpOrder / 2;
    lpcAnalysisFilter(&s_ltp[idx], &channel.out_buf[idx], a_q12, ltp_mem_length - idx, lpc_order);

    int32_t inv_gain_q30 = inverse32VarQ(plc_state.prev_gain_q16[1], 46);
    inv_gain_q30 = std::min(inv_gain_q30, kInt32Max >> 1);
    for (int i = idx + lpc_order; i < ltp_mem_length; ++i) {
        s_ltp_q14[i] = smulwb(inv_gain_q30, s_ltp[i]);
    }

    // LTP synthesis filtering.
    for (int k = 0; k < channel.nb_subfr; ++k) {
        int32_t* pred_lag_ptr = &s_ltp_q14[s_ltp_buf_idx - lag + kLtpOrder / 2];
        for (int i = 0; i < channel.subfr_length; ++i) {
            // The +2 avoids a bias because smlawb() always rounds to -inf.
            int32_t ltp_pred_q12 = 2;
            ltp_pred_q12 = smlawb(ltp_pred_q12, pred_lag_ptr[0], b_q14[0]);
            ltp_pred_q12 = smlawb(ltp_pred_q12, pred_lag_ptr[-1], b_q14[1]);
            ltp_pred_q12 = smlawb(ltp_pred_q12, pred_lag_ptr[-2], b_q14[2]);
            ltp_pred_q12 = smlawb(ltp_pred_q12, pred_lag_ptr[-3], b_q14[3]);
            ltp_pred_q12 = smlawb(ltp_pred_q12, pred_lag_ptr[-4], b_q14[4]);
            pred_lag_ptr++;

            rand_seed = rand32(rand_seed);
            const int rand_idx = (rand_seed >> 25) & kRandBufMask;
            s_ltp_q14[s_ltp_buf_idx] = static_cast<int32_t>(static_cast<uint32_t>(
                smlawb(ltp_pred_q12, rand_ptr[rand_idx], rand_scale_q14)) << 2);
            s_ltp_buf_idx++;
        }

        // Gradually reduce the LTP gain.
        for (int j = 0; j < kLtpOrder; ++j) {
            b_q14[j] = static_cast<int16_t>(smulbb(harm_gain_q15, b_q14[j]) >> 15);
        }
        if (channel.indices.signal_type != kTypeNoVoiceActivity) {
            // Gradually reduce the excitation gain.
            rand_scale_q14 = smulbb(rand_scale_q14, rand_gain_q15) >> 15;
        }

        // Slowly increase the pitch lag.
        plc_state.pitch_l_q8 =
            smlawb(plc_state.pitch_l_q8, plc_state.pitch_l_q8, kPitchDriftFacQ16);
        plc_state.pitch_l_q8 =
            std::min(plc_state.pitch_l_q8, (kMaxPitchLagMs * channel.fs_khz) << 8);
        lag = rshiftRound(plc_state.pitch_l_q8, 8);
    }

    // LPC synthesis filtering.
    int32_t* s_lpc_q14_ptr = &s_ltp_q14[ltp_mem_length - kMaxLpcOrder];
    std::copy(channel.s_lpc_q14_buf, channel.s_lpc_q14_buf + kMaxLpcOrder, s_lpc_q14_ptr);
    for (int i = 0; i < channel.frame_length; ++i) {
        // The +lpc_order/2 avoids a bias because smlawb() always rounds to -inf.
        int32_t lpc_pred_q10 = lpc_order >> 1;
        for (int j = 0; j < lpc_order; ++j) {
            lpc_pred_q10 =
                smlawb(lpc_pred_q10, s_lpc_q14_ptr[kMaxLpcOrder + i - j - 1], a_q12[j]);
        }

        s_lpc_q14_ptr[kMaxLpcOrder + i] =
            addSat32(s_lpc_q14_ptr[kMaxLpcOrder + i], lshiftSat32(lpc_pred_q10, 4));
        frame[i] = static_cast<int16_t>(
            sat16(sat16(rshiftRound(smulww(s_lpc_q14_ptr[kMaxLpcOrder + i], prev_gain_q10[1]), 8))));
    }

    std::copy(s_lpc_q14_ptr + channel.frame_length,
              s_lpc_q14_ptr + channel.frame_length + kMaxLpcOrder, channel.s_lpc_q14_buf);

    plc_state.rand_seed = rand_seed;
    plc_state.rand_scale_q14 = static_cast<int16_t>(rand_scale_q14);
    for (int i = 0; i < kMaxNbSubfr; ++i) {
        control.pitch_l[i] = lag;
    }
}

void SilkDecoder::plcGlueFrames(ChannelState& channel, int16_t* frame, int length) {
    PlcState& plc_state = channel.plc;

    if (channel.loss_cnt) {
        // Energy of the concealed residual.
        sumSqrShift(&plc_state.conc_energy, &plc_state.conc_energy_shift, frame, length);
        plc_state.last_frame_lost = 1;
    } else {
        if (plc_state.last_frame_lost) {
            int32_t energy;
            int energy_shift;
            sumSqrShift(&energy, &energy_shift, frame, length);

            // Normalize the energies.
            if (energy_shift > plc_state.conc_energy_shift) {
                plc_state.conc_energy >>= energy_shift - plc_state.conc_energy_shift;
            } else if (energy_shift < plc_state.conc_energy_shift) {
                energy >>= plc_state.conc_energy_shift - energy_shift;
            }

            // Fade in the energy difference.
            if (energy > plc_state.conc_energy) {
                int32_t lz = clz32(plc_state.conc_energy);
                lz -= 1;
                plc_state.conc_energy <<= lz;
                energy >>= std::max(24 - lz, 0);

                const int32_t frac_q24 = div32(plc_state.conc_energy, std::max(energy, 1));
                int32_t gain_q16 = sqrtApprox(frac_q24) << 4;
                int32_t slope_q16 = div32_16((static_cast<int32_t>(1) << 16) - gain_q16, length);
                // A 4x steeper slope avoids missing onsets after DTX.
                slope_q16 <<= 2;

                for (int i = 0; i < length; ++i) {
                    frame[i] = static_cast<int16_t>(smulwb(gain_q16, frame[i]));
                    gain_q16 += slope_q16;
                    if (gain_q16 > (static_cast<int32_t>(1) << 16)) {
                        break;
                    }
                }
            }
        }
        plc_state.last_frame_lost = 0;
    }
}

void SilkDecoder::cngReset(ChannelState& channel) {
    CngState& cng_state = channel.cng;
    const int32_t nlsf_step_q15 = div32_16(kInt16Max, channel.lpc_order + 1);
    int32_t nlsf_acc_q15 = 0;
    for (int i = 0; i < channel.lpc_order; ++i) {
        nlsf_acc_q15 += nlsf_step_q15;
        cng_state.smth_nlsf_q15[i] = static_cast<int16_t>(nlsf_acc_q15);
    }
    cng_state.smth_gain_q16 = 0;
    cng_state.rand_seed = kCngRandSeedInit;
}

void SilkDecoder::cng(ChannelState& channel, const SilkDecoderControl& control, int16_t* frame,
                      int length) {
    CngState& cng_state = channel.cng;

    if (channel.fs_khz != cng_state.fs_khz) {
        cngReset(channel);
        cng_state.fs_khz = channel.fs_khz;
    }

    if (channel.loss_cnt == 0 && channel.prev_signal_type == kTypeNoVoiceActivity) {
        // Update the CNG parameters.
        for (int i = 0; i < channel.lpc_order; ++i) {
            cng_state.smth_nlsf_q15[i] = static_cast<int16_t>(
                cng_state.smth_nlsf_q15[i] +
                smulwb(static_cast<int32_t>(channel.prev_nlsf_q15[i]) -
                           cng_state.smth_nlsf_q15[i],
                       kCngNlsfSmthQ16));
        }

        // Find the subframe with the highest gain.
        int32_t max_gain_q16 = 0;
        int subfr = 0;
        for (int i = 0; i < channel.nb_subfr; ++i) {
            if (control.gains_q16[i] > max_gain_q16) {
                max_gain_q16 = control.gains_q16[i];
                subfr = i;
            }
        }

        // Update the CNG excitation buffer with the excitation of that subframe.
        std::memmove(cng_state.exc_buf_q14 + channel.subfr_length, cng_state.exc_buf_q14,
                     static_cast<size_t>(channel.nb_subfr - 1) * channel.subfr_length *
                         sizeof(int32_t));
        std::memcpy(cng_state.exc_buf_q14, channel.exc_q14 + subfr * channel.subfr_length,
                    static_cast<size_t>(channel.subfr_length) * sizeof(int32_t));

        for (int i = 0; i < channel.nb_subfr; ++i) {
            cng_state.smth_gain_q16 +=
                smulwb(control.gains_q16[i] - cng_state.smth_gain_q16, kCngGainSmthQ16);
        }
    }

    if (channel.loss_cnt == 0) {
        std::fill(cng_state.synth_state, cng_state.synth_state + channel.lpc_order, 0);
        return;
    }

    // Generate and add the comfort noise.
    int32_t cng_sig_q14[kMaxFrameLength + kMaxLpcOrder];
    int32_t gain_q16 = smulww(channel.plc.rand_scale_q14, channel.plc.prev_gain_q16[1]);
    if (gain_q16 >= (1 << 21) || cng_state.smth_gain_q16 > (1 << 23)) {
        gain_q16 = smultt(gain_q16, gain_q16);
        gain_q16 =
            subLshift32(smultt(cng_state.smth_gain_q16, cng_state.smth_gain_q16), gain_q16, 5);
        gain_q16 = sqrtApprox(gain_q16) << 16;
    } else {
        gain_q16 = smulww(gain_q16, gain_q16);
        gain_q16 =
            subLshift32(smulww(cng_state.smth_gain_q16, cng_state.smth_gain_q16), gain_q16, 5);
        gain_q16 = sqrtApprox(gain_q16) << 8;
    }
    const int32_t gain_q10 = gain_q16 >> 6;

    cngExc(cng_sig_q14 + kMaxLpcOrder, cng_state.exc_buf_q14, length, &cng_state.rand_seed);

    int16_t a_q12[kMaxLpcOrder];
    nlsfToLpc(a_q12, cng_state.smth_nlsf_q15, channel.lpc_order);

    std::copy(cng_state.synth_state, cng_state.synth_state + kMaxLpcOrder, cng_sig_q14);
    for (int i = 0; i < length; ++i) {
        // The +lpc_order/2 avoids a bias because smlawb() always rounds to -inf.
        int32_t lpc_pred_q10 = channel.lpc_order >> 1;
        for (int j = 0; j < channel.lpc_order; ++j) {
            lpc_pred_q10 = smlawb(lpc_pred_q10, cng_sig_q14[kMaxLpcOrder + i - j - 1], a_q12[j]);
        }

        cng_sig_q14[kMaxLpcOrder + i] =
            addSat32(cng_sig_q14[kMaxLpcOrder + i], lshiftSat32(lpc_pred_q10, 4));
        frame[i] = static_cast<int16_t>(addSat16(
            frame[i], sat16(rshiftRound(smulww(cng_sig_q14[kMaxLpcOrder + i], gain_q10), 8))));
    }

    std::copy(cng_sig_q14 + length, cng_sig_q14 + length + kMaxLpcOrder, cng_state.synth_state);
}

}  // namespace player::audio::silk
