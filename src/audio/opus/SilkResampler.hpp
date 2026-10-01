#pragma once

#include <cstdint>

namespace player::audio::silk {

// SILK decoder resampler (silk_resampler). Upsampling only: the decoder
// converts the internal 8/12/16 kHz signal to the output rate.
class SilkResampler {
public:
    // Configures the resampler for the decoder direction. Returns false for
    // unsupported or downsampling configurations.
    bool init(int fs_hz_in, int fs_hz_out);

    // Resamples in_len input samples; the output length is
    // in_len * fs_out / fs_in.
    void process(int16_t* out, const int16_t* in, int in_len);

private:
    enum class Kind {
        Copy,
        Up2Hq,
        IirFir,
    };

    static constexpr int kMaxBatchMs = 10;
    static constexpr int kFirOrder = 8;

    void up2Hq(int16_t* out, const int16_t* in, int len);
    void iirFir(int16_t* out, const int16_t* in, int len);

    Kind kind_ = Kind::Copy;
    int32_t s_iir_[6] = {};
    int16_t s_fir_[kFirOrder] = {};
    int16_t delay_buf_[48] = {};
    int batch_size_ = 0;
    int32_t inv_ratio_q16_ = 0;
    int fs_in_khz_ = 0;
    int fs_out_khz_ = 0;
    int input_delay_ = 0;
};

}  // namespace player::audio::silk
