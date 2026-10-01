#pragma once

namespace player::audio::opus {

// Spread values used by the CELT frame header.
enum class Spread : int {
    None = 0,
    Light = 1,
    Normal = 2,
    Aggressive = 3,
};

// Energy-preserving rotation applied after PVQ decoding; spreads the pulses
// over the band so the spectrum does not concentrate on few samples.
void expRotation(float* x, int len, int dir, int stride, int k, Spread spread);

// Scales the vector so that its energy becomes gain^2 (gain defaults to 1 in
// the decoder). See libopus renormalise_vector.
void renormaliseVector(float* x, int n, float gain);

}  // namespace player::audio::opus
