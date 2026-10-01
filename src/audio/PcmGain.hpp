#pragma once

#include <cstddef>

namespace player::audio {

enum class Mute {
    Off,
    On,
};

constexpr float kMinVolume = 0.0f;
constexpr float kMaxVolume = 1.0f;

float clampVolume(float volume);

// Linear gain for a volume knob position. Muted audio is silent.
float volumeGain(float volume, Mute mute);

// Applies gain to interleaved samples and clamps the result to [-1, 1].
void applyGain(float* samples, size_t count, float gain);

}  // namespace player::audio
