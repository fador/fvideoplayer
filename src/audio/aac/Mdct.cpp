#include "audio/aac/Mdct.hpp"

#include <cmath>
#include <utility>

namespace player::audio::aac {
namespace {

constexpr double kPi = 3.14159265358979323846;

}  // namespace

Mdct::Mdct(size_t bins) : bins_(bins), fft_size_(2 * bins) {
    pre_.resize(bins_);
    for (size_t k = 0; k < bins_; ++k) {
        const double angle = kPi * static_cast<double>(k) * 0.5 +
                             kPi * static_cast<double>(k) / static_cast<double>(fft_size_);
        pre_[k] = std::polar(1.0, angle);
    }

    post_.resize(fft_size_);
    for (size_t n = 0; n < fft_size_; ++n) {
        const double angle = kPi * static_cast<double>(n) / static_cast<double>(fft_size_) +
                             kPi * 0.25 + kPi / static_cast<double>(2 * fft_size_);
        post_[n] = std::polar(1.0, angle);
    }

    work_.resize(fft_size_);
}

// Iterative radix-2 DFT with a positive exponent.
void Mdct::dft(std::complex<double>* data) const {
    const size_t n = fft_size_;

    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; (j & bit) != 0; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(data[i], data[j]);
        }
    }

    for (size_t length = 2; length <= n; length <<= 1) {
        const double angle = 2.0 * kPi / static_cast<double>(length);
        const std::complex<double> step(std::cos(angle), std::sin(angle));
        const size_t half = length >> 1;
        for (size_t start = 0; start < n; start += length) {
            std::complex<double> twiddle(1.0, 0.0);
            for (size_t k = 0; k < half; ++k) {
                const std::complex<double> even = data[start + k];
                const std::complex<double> odd = data[start + k + half] * twiddle;
                data[start + k] = even + odd;
                data[start + k + half] = even - odd;
                twiddle *= step;
            }
        }
    }
}

void Mdct::inverse(const float* spectrum, float* time) const {
    for (size_t k = 0; k < bins_; ++k) {
        work_[k] = static_cast<double>(spectrum[k]) * pre_[k];
    }
    for (size_t k = bins_; k < fft_size_; ++k) {
        work_[k] = std::complex<double>(0.0, 0.0);
    }

    dft(work_.data());

    const double scale = 2.0 / static_cast<double>(bins_);
    for (size_t n = 0; n < fft_size_; ++n) {
        time[n] = static_cast<float>(scale * (work_[n] * post_[n]).real());
    }
}

void Mdct::inverseDirect(const float* spectrum, float* time) const {
    const double scale = 2.0 / static_cast<double>(bins_);
    for (size_t n = 0; n < fft_size_; ++n) {
        double sum = 0.0;
        for (size_t k = 0; k < bins_; ++k) {
            const double angle =
                kPi / static_cast<double>(bins_) *
                (static_cast<double>(n) + 0.5 + static_cast<double>(bins_) * 0.5) *
                (static_cast<double>(k) + 0.5);
            sum += static_cast<double>(spectrum[k]) * std::cos(angle);
        }
        time[n] = static_cast<float>(scale * sum);
    }
}

}  // namespace player::audio::aac
