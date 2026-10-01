#include "audio/opus/SilkResampler.hpp"

#include "audio/opus/SilkMath.hpp"
#include "audio/opus/SilkTables.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace player::audio::silk {
namespace {

// Delay compensation per input/output rate index
// (delay_matrix_dec, resampler.c).
constexpr int kRates[] = {8, 12, 16, 24, 48};
constexpr int8_t kDelayMatrix[3][5] = {
    {4, 0, 2, 0, 0},
    {0, 9, 4, 7, 4},
    {0, 3, 12, 7, 7},
};

int rateIndex(int khz) {
    for (int i = 0; i < 5; ++i) {
        if (kRates[i] == khz) {
            return i;
        }
    }
    return -1;
}

}  // namespace

bool SilkResampler::init(int fs_hz_in, int fs_hz_out) {
    const int in_khz = fs_hz_in / 1000;
    const int out_khz = fs_hz_out / 1000;
    const int in_index = rateIndex(in_khz);
    const int out_index = rateIndex(out_khz);
    if (in_index < 0 || in_index > 2 || out_index < 0) {
        return false;
    }

    *this = SilkResampler{};
    fs_in_khz_ = in_khz;
    fs_out_khz_ = out_khz;
    input_delay_ = kDelayMatrix[in_index][out_index];
    batch_size_ = fs_in_khz_ * kMaxBatchMs;

    int up2x = 0;
    if (fs_hz_out > fs_hz_in) {
        if (fs_hz_out == fs_hz_in * 2) {
            kind_ = Kind::Up2Hq;
        } else {
            kind_ = Kind::IirFir;
            up2x = 1;
        }
    } else if (fs_hz_out < fs_hz_in) {
        return false;  // downsampling is not needed by the decoder
    } else {
        kind_ = Kind::Copy;
    }

    // Input/output ratio in Q16, rounded up.
    inv_ratio_q16_ = ((fs_hz_in << (14 + up2x)) / fs_hz_out) << 2;
    while (smulww(inv_ratio_q16_, fs_hz_out) < (fs_hz_in << up2x)) {
        ++inv_ratio_q16_;
    }
    return true;
}

void SilkResampler::up2Hq(int16_t* out, const int16_t* in, int len) {
    // Internal state is Q10; 2nd order allpass sections plus a notch.
    for (int k = 0; k < len; ++k) {
        const int32_t in32 = static_cast<int32_t>(in[k]) << 10;

        int32_t y = in32 - s_iir_[0];
        int32_t x = smulwb(y, kSilkResamplerUp2Hq0[0]);
        int32_t out32_1 = s_iir_[0] + x;
        s_iir_[0] = in32 + x;

        y = out32_1 - s_iir_[1];
        x = smulwb(y, kSilkResamplerUp2Hq0[1]);
        int32_t out32_2 = s_iir_[1] + x;
        s_iir_[1] = out32_1 + x;

        y = out32_2 - s_iir_[2];
        x = smlawb(y, y, kSilkResamplerUp2Hq0[2]);
        out32_1 = s_iir_[2] + x;
        s_iir_[2] = out32_2 + x;
        out[2 * k] = static_cast<int16_t>(sat16(rshiftRound(out32_1, 10)));

        y = in32 - s_iir_[3];
        x = smulwb(y, kSilkResamplerUp2Hq1[0]);
        out32_1 = s_iir_[3] + x;
        s_iir_[3] = in32 + x;

        y = out32_1 - s_iir_[4];
        x = smulwb(y, kSilkResamplerUp2Hq1[1]);
        out32_2 = s_iir_[4] + x;
        s_iir_[4] = out32_1 + x;

        y = out32_2 - s_iir_[5];
        x = smlawb(y, y, kSilkResamplerUp2Hq1[2]);
        out32_1 = s_iir_[5] + x;
        s_iir_[5] = out32_2 + x;
        out[2 * k + 1] = static_cast<int16_t>(sat16(rshiftRound(out32_1, 10)));
    }
}

void SilkResampler::iirFir(int16_t* out, const int16_t* in, int len) {
    std::array<int16_t, 2 * kMaxBatchMs * 16 + kFirOrder> buf;
    std::copy(s_fir_, s_fir_ + kFirOrder, buf.begin());

    const int32_t index_increment_q16 = inv_ratio_q16_;
    int samples_in = 0;
    for (;;) {
        samples_in = std::min(len, batch_size_);

        // 2x upsampling into the FIR buffer.
        up2Hq(&buf[kFirOrder], in, samples_in);

        // Fractional FIR interpolation.
        const int32_t max_index_q16 = samples_in << 17;  // +1 for the 2x
        for (int32_t index_q16 = 0; index_q16 < max_index_q16;
             index_q16 += index_increment_q16) {
            const int32_t table_index = smulwb(index_q16 & 0xFFFF, 12);
            const int16_t* buf_ptr = &buf[index_q16 >> 16];
            const int16_t* frac = &kSilkResamplerFracFir12[table_index * 4];
            const int16_t* frac_mirror = &kSilkResamplerFracFir12[(11 - table_index) * 4];

            int32_t res_q15 = smulbb(buf_ptr[0], frac[0]);
            res_q15 = smulbb(buf_ptr[1], frac[1]) + res_q15;
            res_q15 = smulbb(buf_ptr[2], frac[2]) + res_q15;
            res_q15 = smulbb(buf_ptr[3], frac[3]) + res_q15;
            res_q15 = smulbb(buf_ptr[4], frac_mirror[3]) + res_q15;
            res_q15 = smulbb(buf_ptr[5], frac_mirror[2]) + res_q15;
            res_q15 = smulbb(buf_ptr[6], frac_mirror[1]) + res_q15;
            res_q15 = smulbb(buf_ptr[7], frac_mirror[0]) + res_q15;
            *out++ = static_cast<int16_t>(sat16(rshiftRound(res_q15, 15)));
        }

        in += samples_in;
        len -= samples_in;
        if (len <= 0) {
            break;
        }
        std::copy(&buf[samples_in << 1], &buf[(samples_in << 1) + kFirOrder], buf.begin());
    }

    std::copy(&buf[samples_in << 1], &buf[(samples_in << 1) + kFirOrder], s_fir_);
}

void SilkResampler::process(int16_t* out, const int16_t* in, int in_len) {
    // The first millisecond goes through the delay buffer.
    const int n_samples = fs_in_khz_ - input_delay_;
    std::copy(in, in + n_samples, &delay_buf_[input_delay_]);

    switch (kind_) {
        case Kind::Up2Hq:
            up2Hq(out, delay_buf_, fs_in_khz_);
            up2Hq(&out[fs_out_khz_], &in[n_samples], in_len - fs_in_khz_);
            break;
        case Kind::IirFir:
            iirFir(out, delay_buf_, fs_in_khz_);
            iirFir(&out[fs_out_khz_], &in[n_samples], in_len - fs_in_khz_);
            break;
        case Kind::Copy:
            std::copy(delay_buf_, delay_buf_ + fs_in_khz_, out);
            std::copy(&in[n_samples], &in[n_samples] + (in_len - fs_in_khz_), &out[fs_out_khz_]);
            break;
    }

    std::copy(&in[in_len - input_delay_], &in[in_len], delay_buf_);
}

}  // namespace player::audio::silk
