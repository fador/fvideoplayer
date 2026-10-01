#pragma once

#include "audio/opus/RangeDecoder.hpp"
#include "audio/opus/SilkCore.hpp"
#include "audio/opus/SilkParameters.hpp"
#include "audio/opus/SilkResampler.hpp"
#include "audio/opus/SilkStereo.hpp"

#include <cstdint>
#include <vector>

namespace player::audio::silk {

// Full SILK decoder: payload parsing, per-frame parameter and excitation
// decoding, LTP/LPC synthesis, stereo unmixing and resampling to 48 kHz
// (silk_Decode from libopus, without packet loss concealment).
class SilkDecoder {
public:
    enum class StartOfPacket {
        No,
        Yes,
    };

    // channels is the API channel count (1 or 2).
    void init(int channels);
    void reset();

    // Decodes one SILK frame of a payload into interleaved float PCM at
    // 48 kHz. fs_khz is the internal rate (8/12/16), stereo the stream channel
    // count and frames the number of SILK frames in the payload (1 for
    // 10/20 ms, 2 for 40 ms, 3 for 60 ms). Must be called once per SILK frame
    // with start marking the first call of each packet.
    void decode(opus::RangeDecoder& decoder, int fs_khz, int stereo, int frames, int frame_ms,
                StartOfPacket start, float* pcm);

private:
    struct ChannelState {
        int fs_khz = 0;
        int nb_subfr = 0;
        int subfr_length = 0;
        int frame_length = 0;
        int ltp_mem_length = 0;
        int lpc_order = 0;
        const NlsfCodebook* nlsf_codebook = nullptr;
        const uint8_t* pitch_lag_low_bits_icdf = nullptr;
        const uint8_t* pitch_contour_icdf = nullptr;
        int n_frames_decoded = 0;
        int n_frames_per_packet = 0;
        uint8_t vad_flags[3] = {};
        uint8_t lbrr_flags[3] = {};
        int lbrr_flag = 0;
        int ec_prev_signal_type = 0;
        int ec_prev_lag_index = 0;
        SilkIndices indices;
        int16_t prev_nlsf_q15[kMaxLpcOrder] = {};
        int8_t last_gain_index = 10;
        int first_frame_after_reset = 1;
        int prev_signal_type = 0;
        int lag_prev = 100;
        int32_t prev_gain_q16 = 65536;
        int32_t s_lpc_q14_buf[kMaxLpcOrder] = {};
        std::vector<int16_t> out_buf;
        SilkResampler resampler;
    };

    void setFs(ChannelState& channel, int fs_khz, int nb_subfr);
    void decodeFrame(ChannelState& channel, opus::RangeDecoder& decoder, int16_t* out,
                     CondCoding cond_coding);
    static SilkState makeIndState(const ChannelState& channel);

    int api_channels_ = 0;
    ChannelState channels_[2];
    SilkStereoState stereo_;
    int prev_decode_only_middle_ = 0;
    int prev_channels_ = 0;
    int prev_internal_ = 0;
};

}  // namespace player::audio::silk
