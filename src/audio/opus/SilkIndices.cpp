#include "audio/opus/SilkIndices.hpp"

#include "audio/opus/SilkNlsf.hpp"

namespace player::audio::silk {
namespace {

constexpr int kQuantMaxAmplitude = 4;  // NLSF_QUANT_MAX_AMPLITUDE

}  // namespace

void decodeIndices(SilkState& state, opus::RangeDecoder& decoder, int frame_index, DecodeLbrr decode_lbrr,
                   CondCoding cond_coding, SilkIndices& indices) {
    // Signal type and quantizer offset.
    int ix;
    if (decode_lbrr == DecodeLbrr::Yes || state.vad_flags[frame_index] != 0) {
        ix = decoder.decodeIcdf(kSilkTypeOffsetVadIcdf, 8) + 2;
    } else {
        ix = decoder.decodeIcdf(kSilkTypeOffsetNoVadIcdf, 8);
    }
    indices.signal_type = static_cast<int8_t>(ix >> 1);
    indices.quant_offset_type = static_cast<int8_t>(ix & 1);

    // Gains: the first subframe is either delta coded or split into MSB/LSB.
    if (cond_coding == CondCoding::Conditionally) {
        indices.gains_indices[0] = static_cast<int8_t>(decoder.decodeIcdf(kSilkDeltaGainIcdf, 8));
    } else {
        indices.gains_indices[0] = static_cast<int8_t>(
            decoder.decodeIcdf(&kSilkGainIcdf[indices.signal_type * 8], 8) << 3);
        indices.gains_indices[0] = static_cast<int8_t>(
            indices.gains_indices[0] + decoder.decodeIcdf(kSilkUniform8Icdf, 8));
    }
    for (int i = 1; i < state.nb_subfr; ++i) {
        indices.gains_indices[i] = static_cast<int8_t>(decoder.decodeIcdf(kSilkDeltaGainIcdf, 8));
    }

    // NLSF indices: first stage, then the residuals with an extension range.
    const NlsfCodebook& cb = *state.nlsf_codebook;
    indices.nlsf_indices[0] = static_cast<int8_t>(
        decoder.decodeIcdf(&cb.cb1_icdf[(indices.signal_type >> 1) * cb.n_vectors], 8));
    int16_t ec_ix[kMaxLpcOrder];
    uint8_t pred_q8[kMaxLpcOrder];
    nlsfUnpack(ec_ix, pred_q8, cb, indices.nlsf_indices[0]);
    for (int i = 0; i < cb.order; ++i) {
        ix = decoder.decodeIcdf(&cb.ec_icdf[ec_ix[i]], 8);
        if (ix == 0) {
            ix -= decoder.decodeIcdf(kSilkNlsfExtIcdf, 8);
        } else if (ix == 2 * kQuantMaxAmplitude) {
            ix += decoder.decodeIcdf(kSilkNlsfExtIcdf, 8);
        }
        indices.nlsf_indices[i + 1] = static_cast<int8_t>(ix - kQuantMaxAmplitude);
    }

    // NLSF interpolation factor (only for 20 ms frames).
    if (state.nb_subfr == kMaxNbSubfr) {
        indices.nlsf_interp_coef_q2 = static_cast<int8_t>(decoder.decodeIcdf(kSilkNlsfInterpIcdf, 8));
    } else {
        indices.nlsf_interp_coef_q2 = 4;
    }

    if (indices.signal_type == kTypeVoiced) {
        // Pitch lags, absolute or delta coded against the previous frame.
        bool decode_absolute_lag_index = true;
        if (cond_coding == CondCoding::Conditionally &&
            state.ec_prev_signal_type == kTypeVoiced) {
            int delta_lag_index = decoder.decodeIcdf(kSilkPitchDeltaIcdf, 8);
            if (delta_lag_index > 0) {
                delta_lag_index -= 9;
                indices.lag_index =
                    static_cast<int16_t>(state.ec_prev_lag_index + delta_lag_index);
                decode_absolute_lag_index = false;
            }
        }
        if (decode_absolute_lag_index) {
            indices.lag_index = static_cast<int16_t>(
                decoder.decodeIcdf(kSilkPitchLagIcdf, 8) * (state.fs_khz >> 1));
            indices.lag_index = static_cast<int16_t>(
                indices.lag_index + decoder.decodeIcdf(state.pitch_lag_low_bits_icdf, 8));
        }
        state.ec_prev_lag_index = indices.lag_index;

        indices.contour_index = static_cast<int8_t>(decoder.decodeIcdf(state.pitch_contour_icdf, 8));

        // LTP gains and scaling.
        indices.per_index = static_cast<int8_t>(decoder.decodeIcdf(kSilkLtpPerIndexIcdf, 8));
        for (int k = 0; k < state.nb_subfr; ++k) {
            indices.ltp_index[k] =
                static_cast<int8_t>(decoder.decodeIcdf(kSilkLtpGainIcdf[indices.per_index], 8));
        }
        if (cond_coding == CondCoding::Independently) {
            indices.ltp_scale_index = static_cast<int8_t>(decoder.decodeIcdf(kSilkLtpScaleIcdf, 8));
        } else {
            indices.ltp_scale_index = 0;
        }
    }
    state.ec_prev_signal_type = indices.signal_type;

    indices.seed = static_cast<int8_t>(decoder.decodeIcdf(kSilkUniform4Icdf, 8));
}

}  // namespace player::audio::silk
