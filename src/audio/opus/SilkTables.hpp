#pragma once

#include <cstdint>

namespace player::audio::silk {

// NLSF codebook (the subset of silk_NLSF_CB_struct needed by the decoder).
struct NlsfCodebook {
    int n_vectors;
    int order;
    int quant_step_size_q16;
    int inv_quant_step_size_q6;
    const uint8_t* cb1_nlsf_q8;
    const int16_t* cb1_wght_q9;
    const uint8_t* cb1_icdf;
    const uint8_t* pred_q8;
    const uint8_t* ec_sel;
    const uint8_t* ec_icdf;
    const uint8_t* ec_rates_q5;
    const int16_t* delta_min_q15;
};

extern const NlsfCodebook kNlsfCodebookNbMb;
extern const NlsfCodebook kNlsfCodebookWb;

// Side information inverse CDFs (tables_other.c, tables_gain.c, tables_LTP.c,
// tables_pitch_lag.c).
extern const uint8_t kSilkTypeOffsetVadIcdf[4];
extern const uint8_t kSilkTypeOffsetNoVadIcdf[2];
extern const uint8_t kSilkDeltaGainIcdf[41];
extern const uint8_t kSilkGainIcdf[3 * 8];
extern const uint8_t kSilkUniform4Icdf[4];
extern const uint8_t kSilkUniform6Icdf[6];
extern const uint8_t kSilkUniform8Icdf[8];
extern const uint8_t kSilkNlsfExtIcdf[7];
extern const uint8_t kSilkNlsfInterpIcdf[5];
extern const uint8_t kSilkPitchDeltaIcdf[21];
extern const uint8_t kSilkPitchLagIcdf[32];
extern const uint8_t kSilkLtpPerIndexIcdf[3];
extern const uint8_t kSilkLtpGainIcdf0[8];
extern const uint8_t kSilkLtpGainIcdf1[16];
extern const uint8_t kSilkLtpGainIcdf2[32];
extern const uint8_t* const kSilkLtpGainIcdf[3];
extern const uint8_t kSilkLtpScaleIcdf[3];
extern const uint8_t kSilkPitchContourIcdf[34];
extern const uint8_t kSilkPitchContourNbIcdf[11];
extern const uint8_t kSilkPitchContour10msIcdf[12];
extern const uint8_t kSilkPitchContour10msNbIcdf[3];

// Excitation and pitch tables (tables_pulses_per_block.c, pitch_est_tables.c).
extern const uint8_t kSilkRateLevelsIcdf[2 * 9];
extern const uint8_t kSilkPulsesPerBlockIcdf[10 * 18];
extern const uint8_t kSilkShellCodeTable0[152];
extern const uint8_t kSilkShellCodeTable1[152];
extern const uint8_t kSilkShellCodeTable2[152];
extern const uint8_t kSilkShellCodeTable3[152];
extern const uint8_t kSilkShellCodeOffsets[17];
extern const uint8_t kSilkSignIcdf[42];
extern const uint8_t kSilkLsbIcdf[2];
extern const int8_t kSilkCbLagsStage2[4 * 11];
extern const int8_t kSilkCbLagsStage3[4 * 34];
extern const int8_t kSilkCbLagsStage2_10ms[2 * 3];
extern const int8_t kSilkCbLagsStage3_10ms[2 * 12];

}  // namespace player::audio::silk
