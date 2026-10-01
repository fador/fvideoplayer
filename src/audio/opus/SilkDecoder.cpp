#include "audio/opus/SilkDecoder.hpp"

#include "audio/opus/SilkExcitation.hpp"
#include "audio/opus/SilkIndices.hpp"

#include <algorithm>

namespace player::audio::silk {
namespace {

constexpr float kPcmScale = 1.0f / 32768.0f;

}  // namespace

SilkState SilkDecoder::makeIndState(const ChannelState& channel) {
    SilkState state;
    state.fs_khz = channel.fs_khz;
    state.nb_subfr = channel.nb_subfr;
    state.nlsf_codebook = channel.nlsf_codebook;
    state.pitch_lag_low_bits_icdf = channel.pitch_lag_low_bits_icdf;
    state.pitch_contour_icdf = channel.pitch_contour_icdf;
    state.vad_flags = channel.vad_flags;
    state.ec_prev_signal_type = channel.ec_prev_signal_type;
    state.ec_prev_lag_index = channel.ec_prev_lag_index;
    return state;
}

void SilkDecoder::init(int channels) {
    api_channels_ = channels;
    reset();
}

void SilkDecoder::reset() {
    for (auto& channel : channels_) {
        channel = ChannelState{};
    }
    stereo_ = SilkStereoState{};
    prev_decode_only_middle_ = 0;
}

void SilkDecoder::setFs(ChannelState& channel, int fs_khz, int nb_subfr) {
    const int subfr_length = 5 * fs_khz;
    const int frame_length = nb_subfr * subfr_length;
    const bool fs_changed = channel.fs_khz != fs_khz;

    if (fs_changed) {
        channel.resampler.init(fs_khz * 1000, 48000);
    }

    if (fs_changed || frame_length != channel.frame_length) {
        // Pitch contour table depends on the bandwidth and frame size.
        if (fs_khz == 8) {
            channel.pitch_contour_icdf =
                nb_subfr == 4 ? kSilkPitchContourNbIcdf : kSilkPitchContour10msNbIcdf;
        } else {
            channel.pitch_contour_icdf =
                nb_subfr == 4 ? kSilkPitchContourIcdf : kSilkPitchContour10msIcdf;
        }
        if (fs_changed) {
            channel.ltp_mem_length = 20 * fs_khz;
            channel.lpc_order = fs_khz == 16 ? kMaxLpcOrder : 10;
            channel.nlsf_codebook = fs_khz == 16 ? &kNlsfCodebookWb : &kNlsfCodebookNbMb;
            channel.pitch_lag_low_bits_icdf = fs_khz == 16
                                                  ? kSilkUniform8Icdf
                                                  : (fs_khz == 12 ? kSilkUniform6Icdf
                                                                  : kSilkUniform4Icdf);
            channel.first_frame_after_reset = 1;
            channel.lag_prev = 100;
            channel.last_gain_index = 10;
            channel.prev_signal_type = kTypeNoVoiceActivity;
            channel.out_buf.assign(channel.ltp_mem_length + 2 * subfr_length, 0);
            std::fill(channel.s_lpc_q14_buf, channel.s_lpc_q14_buf + kMaxLpcOrder, 0);
        }
        channel.fs_khz = fs_khz;
        channel.nb_subfr = nb_subfr;
        channel.subfr_length = subfr_length;
        channel.frame_length = frame_length;
    }
}

void SilkDecoder::decodeFrame(ChannelState& channel, opus::RangeDecoder& decoder, int16_t* out,
                              CondCoding cond_coding) {
    std::vector<int16_t> pulses((channel.frame_length + 15) & ~15);

    SilkState ind_state = makeIndState(channel);
    decodeIndices(ind_state, decoder, channel.n_frames_decoded, DecodeLbrr::No, cond_coding,
                  channel.indices);
    channel.ec_prev_signal_type = ind_state.ec_prev_signal_type;
    channel.ec_prev_lag_index = ind_state.ec_prev_lag_index;

    decodePulses(decoder, pulses.data(), channel.indices.signal_type,
                 channel.indices.quant_offset_type, channel.frame_length);

    SilkParamState params;
    params.lpc_order = channel.lpc_order;
    params.nb_subfr = channel.nb_subfr;
    params.fs_khz = channel.fs_khz;
    params.nlsf_codebook = channel.nlsf_codebook;
    std::copy(channel.prev_nlsf_q15, channel.prev_nlsf_q15 + kMaxLpcOrder, params.prev_nlsf_q15);
    params.last_gain_index = channel.last_gain_index;
    params.first_frame_after_reset = channel.first_frame_after_reset;
    params.loss_count = 0;

    SilkDecoderControl control;
    decodeParameters(params, channel.indices, cond_coding, control);

    SilkCoreState core;
    core.lpc_order = channel.lpc_order;
    core.nb_subfr = channel.nb_subfr;
    core.subfr_length = channel.subfr_length;
    core.frame_length = channel.frame_length;
    core.ltp_mem_length = channel.ltp_mem_length;
    core.signal_type = channel.indices.signal_type;
    core.quant_offset_type = channel.indices.quant_offset_type;
    core.nlsf_interp_coef_q2 = channel.indices.nlsf_interp_coef_q2;
    core.seed = channel.indices.seed;
    core.loss_count = 0;
    core.prev_signal_type = channel.prev_signal_type;
    core.lag_prev = channel.lag_prev;
    core.prev_gain_q16 = channel.prev_gain_q16;
    std::copy(channel.s_lpc_q14_buf, channel.s_lpc_q14_buf + kMaxLpcOrder, core.s_lpc_q14_buf);
    core.out_buf = channel.out_buf;

    decodeCore(core, control, out, pulses.data());

    channel.prev_gain_q16 = core.prev_gain_q16;
    std::copy(core.s_lpc_q14_buf, core.s_lpc_q14_buf + kMaxLpcOrder, channel.s_lpc_q14_buf);
    channel.out_buf = core.out_buf;
    std::copy(params.prev_nlsf_q15, params.prev_nlsf_q15 + kMaxLpcOrder, channel.prev_nlsf_q15);
    channel.last_gain_index = params.last_gain_index;
    channel.prev_signal_type = channel.indices.signal_type;
    channel.first_frame_after_reset = 0;
    channel.lag_prev = control.pitch_l[channel.nb_subfr - 1];


    // Shift the output history and append this frame.
    const int move_len = channel.ltp_mem_length - channel.frame_length;
    std::copy(channel.out_buf.begin() + channel.frame_length,
              channel.out_buf.begin() + channel.frame_length + move_len, channel.out_buf.begin());
    std::copy(out, out + channel.frame_length, channel.out_buf.begin() + move_len);
}

void SilkDecoder::decode(opus::RangeDecoder& decoder, int fs_khz, int stereo, int frames,
                         int frame_ms, StartOfPacket start, float* pcm) {
    const int n_ch = stereo;

    if (start == StartOfPacket::Yes) {
        // A stereo stream after a mono one reuses the mono state for channel 0.
        if (n_ch > prev_channels_) {
            channels_[1] = ChannelState{};
        }

        for (int n = 0; n < n_ch; ++n) {
            channels_[n].n_frames_decoded = 0;
        }

        const int nb_subfr = frame_ms == 10 ? 2 : 4;
        for (int n = 0; n < n_ch; ++n) {
            channels_[n].n_frames_per_packet = frames;
            setFs(channels_[n], fs_khz, nb_subfr);
        }

        if (n_ch == 2 && (prev_channels_ == 1 || prev_internal_ == 1)) {
            std::fill(std::begin(stereo_.pred_prev_q13), std::end(stereo_.pred_prev_q13), 0);
            std::fill(std::begin(stereo_.s_side), std::end(stereo_.s_side), static_cast<int16_t>(0));
            // The second channel reuses the first one's resampler state.
            channels_[1].resampler = channels_[0].resampler;
        }
        prev_channels_ = n_ch;
        prev_internal_ = n_ch;

        // VAD and LBRR flags.
        for (int n = 0; n < n_ch; ++n) {
            for (int i = 0; i < frames; ++i) {
                channels_[n].vad_flags[i] = decoder.decodeBitLogp(1) ? 1 : 0;
            }
            channels_[n].lbrr_flag = decoder.decodeBitLogp(1) ? 1 : 0;
        }
        for (int n = 0; n < n_ch; ++n) {
            std::fill(std::begin(channels_[n].lbrr_flags), std::end(channels_[n].lbrr_flags), static_cast<uint8_t>(0));
            if (channels_[n].lbrr_flag == 0) {
                continue;
            }
            if (frames == 1) {
                channels_[n].lbrr_flags[0] = 1;
            } else {
                const int symbol =
                    decoder.decodeIcdf(frames == 2 ? kSilkLbrrFlags2Icdf : kSilkLbrrFlags3Icdf, 8) +
                    1;
                for (int i = 0; i < frames; ++i) {
                    channels_[n].lbrr_flags[i] = (symbol >> i) & 1;
                }
            }
        }

        // Skip the LBRR data.
        for (int i = 0; i < frames; ++i) {
            for (int n = 0; n < n_ch; ++n) {
                if (channels_[n].lbrr_flags[i] == 0) {
                    continue;
                }
                if (n_ch == 2 && n == 0) {
                    int32_t ms_pred[2];
                    decodeStereoPred(decoder, ms_pred);
                    if (channels_[1].lbrr_flags[i] == 0) {
                        decodeMidOnly(decoder);
                    }
                }
                const CondCoding cond =
                    (i > 0 && channels_[n].lbrr_flags[i - 1]) ? CondCoding::Conditionally
                                                              : CondCoding::Independently;
                SilkState ind_state = makeIndState(channels_[n]);
                decodeIndices(ind_state, decoder, i, DecodeLbrr::Yes, cond, channels_[n].indices);
                channels_[n].ec_prev_signal_type = ind_state.ec_prev_signal_type;
                channels_[n].ec_prev_lag_index = ind_state.ec_prev_lag_index;
                int16_t pulses[320];
                decodePulses(decoder, pulses, channels_[n].indices.signal_type,
                             channels_[n].indices.quant_offset_type, channels_[n].frame_length);
            }
        }
    }

    // Mid/side predictors for this frame.
    int32_t ms_pred[2] = {0, 0};
    int decode_only_middle = 0;
    if (n_ch == 2) {
        decodeStereoPred(decoder, ms_pred);
        if (channels_[1].vad_flags[channels_[0].n_frames_decoded] == 0) {
            decode_only_middle = decodeMidOnly(decoder);
        }
    }

    // Reset the side channel when side coding resumes.
    if (n_ch == 2 && decode_only_middle == 0 && prev_decode_only_middle_ == 1) {
        std::fill(channels_[1].out_buf.begin(), channels_[1].out_buf.end(), static_cast<int16_t>(0));
        std::fill(channels_[1].s_lpc_q14_buf, channels_[1].s_lpc_q14_buf + kMaxLpcOrder, 0);
        channels_[1].lag_prev = 100;
        channels_[1].last_gain_index = 10;
        channels_[1].prev_signal_type = kTypeNoVoiceActivity;
        channels_[1].first_frame_after_reset = 1;
    }

    const bool has_side = decode_only_middle == 0;
    const int frame_len = channels_[0].frame_length;
    std::vector<int16_t> samples[2];
    samples[0].assign(frame_len + 3, 0);
    samples[1].assign(frame_len + 3, 0);

    for (int n = 0; n < n_ch; ++n) {
        if (n == 0 || has_side) {
            const int frame_index = channels_[0].n_frames_decoded - n;
            CondCoding cond;
            if (frame_index <= 0) {
                cond = CondCoding::Independently;
            } else if (n > 0 && prev_decode_only_middle_ == 1) {
                cond = CondCoding::IndependentlyNoLtpScaling;
            } else {
                cond = CondCoding::Conditionally;
            }
            decodeFrame(channels_[n], decoder, &samples[n][2], cond);
        } else {
            std::fill(&samples[n][2], &samples[n][2] + frame_len, static_cast<int16_t>(0));
        }
        channels_[n].n_frames_decoded++;
    }

    // Stereo unmixing or the mono inter-frame state.
    if (n_ch == 2) {
        stereoMsToLr(stereo_, samples[0].data(), samples[1].data(), ms_pred,
                     channels_[0].fs_khz, frame_len);
    } else {
        samples[0][0] = stereo_.s_mid[0];
        samples[0][1] = stereo_.s_mid[1];
        stereo_.s_mid[0] = samples[0][frame_len];
        stereo_.s_mid[1] = samples[0][frame_len + 1];
    }

    // Resample to 48 kHz and interleave.
    const int out_samples = frame_len * 48000 / (channels_[0].fs_khz * 1000);
    std::vector<int16_t> resampled(out_samples);
    if (n_ch == 2) {
        for (int n = 0; n < 2; ++n) {
            channels_[n].resampler.process(resampled.data(), &samples[n][1], frame_len);
            for (int i = 0; i < out_samples; ++i) {
                pcm[2 * i + n] = static_cast<float>(resampled[i]) * kPcmScale;
            }
        }
    } else {
        channels_[0].resampler.process(resampled.data(), &samples[0][1], frame_len);
        for (int i = 0; i < out_samples; ++i) {
            pcm[i] = static_cast<float>(resampled[i]) * kPcmScale;
        }
    }

    prev_decode_only_middle_ = decode_only_middle;
}

}  // namespace player::audio::silk


