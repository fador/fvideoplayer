#pragma once

namespace player::audio::opus {

constexpr int kCeltLpcOrder = 24;         // PLC synthesis filter order
constexpr int kCeltMaxPeriod = 1024;      // longest PLC pitch period
constexpr int kCeltDecodeBufferSize = 2048;
constexpr int kPlcPitchLagMax = 720;
constexpr int kPlcPitchLagMin = 100;

// Float LPC helpers and the pitch search used by CELT packet-loss
// concealment (celt_lpc.c and pitch.c in libopus).
void celtAutocorr(const float* x, float* ac, const float* window, int overlap, int lag, int n);
void celtLpc(float* lpc, const float* ac, int p);
void celtFir(const float* x, const float* num, float* y, int n, int ord);
void celtIir(const float* x, const float* den, float* y, int n, int ord, float* mem);
float celtInnerProd(const float* x, const float* y, int n);
void celtPitchXcorr(const float* x, const float* y, float* xcorr, int len, int max_pitch);

// Downsamples the decoder history and searches the pitch period of the last
// decoded frame; returns a lag in samples at 48 kHz.
int plcPitchSearch(float* const decode_mem[2], int channels);

}  // namespace player::audio::opus
