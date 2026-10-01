#pragma once

#include <cstddef>
#include <cstdint>

namespace player::audio::simd {

// Runtime CPU detection (CPUID + XGETBV). True when AVX2 is usable.
bool hasAvx2();

// q^(4/3) for q = 0..8191: the AAC inverse quantization magnitudes.
constexpr size_t kPow34TableSize = 8192;
const float* pow34Table();

//    Hot audio kernels. Each entry point dispatches to the AVX2
//    implementation when the CPU supports it, otherwise to the scalar one.
//    The scalar and AVX2 variants are exposed for tests.

void applyGain(float* samples, size_t count, float gain);
void dequantizeBand(const int16_t* quant, float* spectrum, size_t count, float gain);
void windowMultiply(float* block, const float* window, size_t count);
void addWindowed(float* dst, const float* src, const float* window, size_t count);
void scaleCopy(float* dst, const float* src, size_t count, float scale);
void midSide(float* left, float* right, size_t count);
// Multiplies interleaved complex doubles by interleaved complex twiddles.
void complexMultiply(double* data, const double* twiddles, size_t count);
// Positive-exponent, unnormalized DFT of interleaved complex doubles.
void dft(double* interleaved, size_t size);

void applyGainScalar(float* samples, size_t count, float gain);
void applyGainAvx2(float* samples, size_t count, float gain);
void dequantizeBandScalar(const int16_t* quant, float* spectrum, size_t count, float gain);
void dequantizeBandAvx2(const int16_t* quant, float* spectrum, size_t count, float gain);
void windowMultiplyScalar(float* block, const float* window, size_t count);
void windowMultiplyAvx2(float* block, const float* window, size_t count);
void addWindowedScalar(float* dst, const float* src, const float* window, size_t count);
void addWindowedAvx2(float* dst, const float* src, const float* window, size_t count);
void scaleCopyScalar(float* dst, const float* src, size_t count, float scale);
void scaleCopyAvx2(float* dst, const float* src, size_t count, float scale);
void midSideScalar(float* left, float* right, size_t count);
void midSideAvx2(float* left, float* right, size_t count);
void complexMultiplyScalar(double* data, const double* twiddles, size_t count);
void complexMultiplyAvx2(double* data, const double* twiddles, size_t count);
void dftScalar(double* interleaved, size_t size);
void dftAvx2(double* interleaved, size_t size);

}  // namespace player::audio::simd
