#include "audio/PcmGain.hpp"

#include "audio/Simd.hpp"

#include <algorithm>

namespace player::audio {

float clampVolume(float volume) {
    return std::clamp(volume, kMinVolume, kMaxVolume);
}

float volumeGain(float volume, Mute mute) {
    //    volume knob      gain
    //    0.0 (left)  ->   0.0  silence
    //    0.5         ->   0.5  -6 dB
    //    1.0 (right) ->   1.0  0 dB
    return mute == Mute::On ? 0.0f : clampVolume(volume);
}

void applyGain(float* samples, size_t count, float gain) {
    if (gain == 1.0f) {
        return;
    }
    simd::applyGain(samples, count, gain);
}

}  // namespace player::audio
