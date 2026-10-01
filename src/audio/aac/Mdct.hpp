#pragma once

#include <cstddef>
#include <complex>
#include <vector>

namespace player::audio::aac {

// Inverse MDCT for AAC block sizes: N spectral bins -> 2N time samples.
//
//   x[n] = 2/N * sum_{k=0}^{N-1} X[k] * cos(pi/N * (n + 0.5 + N/2) * (k + 0.5))
//
// Evaluated as one complex DFT of size 2N:
//
//   a[k]   = X[k] * i^k * e^(i*pi*k/(2N))          (k < N, else 0)
//   A[n]   = sum_k a[k] * e^(i*2*pi*n*k/(2N))      (positive-sign DFT)
//   x[n]   = 2/N * Re{ A[n] * e^(i*(pi*n/(2N) + pi/4 + pi/(4N))) }
class Mdct {
public:
    explicit Mdct(size_t bins);

    size_t bins() const { return bins_; }

    // spectrum: bins() floats; time: 2 * bins() floats.
    void inverse(const float* spectrum, float* time) const;

    // Direct O(N^2) evaluation of the definition; test reference only.
    void inverseDirect(const float* spectrum, float* time) const;

private:
    void dft(std::complex<double>* data) const;

    size_t bins_ = 0;
    size_t fft_size_ = 0;
    std::vector<std::complex<double>> pre_;
    std::vector<std::complex<double>> post_;
    mutable std::vector<std::complex<double>> work_;
};

}  // namespace player::audio::aac
