#pragma once

#include "audio/BitReader.hpp"
#include "audio/Pcm.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace player::audio::ac3 {

enum class Status {
    Ok,
    Unsupported,  // valid bitstream, but a feature this decoder does not implement
    Invalid,      // corrupt or truncated input
};

// From-scratch AC-3 (Dolby Digital, ATSC A/52) decoder.
//
// Input is one AC-3 syncframe; output is interleaved float PCM at the stream's
// sample rate with the stream's full channel count. Channels are emitted in
// WAVE order (FL, FR, FC, LFE, BL, BR), which is what the sink expects.
//
// Implemented: bitstream id 1..10 (plain AC-3 and the half-rate variants), all
// audio coding modes and LFE, coupling, stereo rematrixing, dither, dynamic
// range, the standard bit allocation, all mantissa quantisers and both block
// transforms. E-AC-3 (bsid > 10), AHT and spectral extension are rejected.
class Decoder {
public:
    // AC-3 has no extradata; an empty config is the only accepted one.
    bool configure(const uint8_t* config, size_t size, std::string& error);

    bool configured() const { return configured_; }
    uint32_t sampleRate() const { return sample_rate_; }
    uint32_t channels() const { return channels_; }

    Status decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error);

    // Clears the overlap history and the dither sequence; used after seeking.
    void flush();

    void reset();

private:
    struct Header {
        int bitstream_id = 0;
        int sr_code = 0;
        int sr_shift = 0;
        int bit_rate = 0;
        int frame_size = 0;
        int channel_mode = 0;
        int bitstream_mode = 0;
        bool lfe_on = false;
        uint32_t sample_rate = 0;
        int channels = 0;
        int center_mix_level = 5;
        int surround_mix_level = 6;
        int dialog_normalization[2] = {0, 0};
        bool compression_exists[2] = {false, false};
        int heavy_dynamic_range[2] = {0, 0};
        bool eac3 = false;
    };

    bool parseHeader(BitReader& reader, Header& header);
    bool decodeBlock(BitReader& reader, int blk);
    bool couplingStrategy(BitReader& reader, int blk);
    bool couplingCoordinates(BitReader& reader, int blk);
    bool decodeExponents(BitReader& reader, int strategy, int groups, int absolute,
                         int8_t* exponents);
    bool decodeTransformCoeffs(BitReader& reader, int blk);
    void calcTransformCoeffsCpl();
    void removeDithering();
    void doRematrixing();
    void doImdct(int ch, float* out);
    void bitAllocCalcPsd(int ch);
    bool bitAllocCalcMask(int ch);
    void bitAllocCalcBap(int ch);
    void resetState();
    uint32_t nextDither();

    bool configured_ = false;
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 0;
    bool have_config_ = false;

    // Frame header state.
    int bitstream_id_ = 0;
    int bitstream_mode_ = 0;
    int channel_mode_ = 0;
    bool lfe_on_ = false;
    int fbw_channels_ = 0;
    int lfe_ch_ = 0;
    int frame_size_ = 0;
    int sr_code_ = 0;
    int sr_shift_ = 0;

    // Volume control.
    float dynamic_range_[2] = {1.0f, 1.0f};

    // Bit allocation parameters.
    int slow_decay_ = 0;
    int fast_decay_ = 0;
    int slow_gain_ = 0;
    int db_per_bit_ = 0;
    int floor_ = 0;
    int cpl_fast_leak_ = 0;
    int cpl_slow_leak_ = 0;

    // Per-channel state, indexed by the decoder channel (0 = coupling, 1..6).
    int block_switch_[7] = {};
    int dither_flag_[7] = {};
    int exp_strategy_[6][7] = {};
    int8_t dexps_[7][256] = {};
    uint8_t bap_[7][256] = {};
    int16_t psd_[7][256] = {};
    int16_t band_psd_[7][50] = {};
    int16_t mask_[7][50] = {};
    int start_freq_[7] = {};
    int end_freq_[7] = {};
    int num_exp_groups_[7] = {};
    int snr_offset_[7] = {};
    int fast_gain_[7] = {};
    int dba_mode_[7] = {};
    int dba_nsegs_[7] = {};
    uint8_t dba_offsets_[7][8] = {};
    uint8_t dba_lengths_[7][8] = {};
    uint8_t dba_values_[7][8] = {};
    int channel_in_cpl_[7] = {};
    bool cpl_in_use_[6] = {};
    bool phase_flags_in_use_ = false;
    int phase_flags_[18] = {};
    int num_cpl_bands_ = 0;
    uint8_t cpl_band_sizes_[18] = {};
    bool first_cpl_coords_[7] = {};
    int cpl_coords_[7][18] = {};
    bool first_cpl_leak_ = false;
    int num_rematrixing_bands_ = 0;
    int rematrixing_flags_[4] = {};

    // Sample state.
    float coeffs_[7][256] = {};
    float transform_coeffs_[7][256] = {};
    float delay_[7][256] = {};
    float block_out_[7][1536] = {};
    uint32_t dither_state_ = 0;
};

}  // namespace player::audio::ac3
