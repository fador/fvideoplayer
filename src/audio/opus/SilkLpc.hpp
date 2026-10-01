#pragma once

#include <cstdint>

namespace player::audio::silk {

// NLSF to LPC conversion and prediction filter helpers.

// Converts normalized line spectral frequencies to monic whitening filter
// coefficients in Q12 (silk_NLSF2A). order is 10 or 16.
void nlsfToLpc(int16_t* a_q12, const int16_t* nlsf, int order);

// Inverse prediction gain in Q30, 0 when unstable
// (silk_LPC_inverse_pred_gain).
int32_t lpcInversePredGain(const int16_t* a_q12, int order);

// Scales int32 coefficients into int16, bandwidth expanding when needed
// (silk_LPC_fit).
void lpcFit(int16_t* a_qout, int32_t* a_qin, int qout, int qin, int d);

// Chirp (bandwidth expand) the filter (silk_bwexpander / bwexpander_32).
void bwexpander(int16_t* ar, int d, int32_t chirp_q16);
void bwexpander32(int32_t* ar, int d, int32_t chirp_q16);

// Interpolates two vectors (silk_interpolate).
void interpolate(int16_t* xi, const int16_t* x0, const int16_t* x1, int ifact_q2, int d);

}  // namespace player::audio::silk
