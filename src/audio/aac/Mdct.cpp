#include "audio/aac/Mdct.hpp"

#include "audio/Simd.hpp"

#include <cmath>

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

void Mdct::inverse(const float* spectrum, float* time) const {
    for (size_t k = 0; k < bins_; ++k) {
        work_[k] = static_cast<double>(spectrum[k]) * pre_[k];
    }
    for (size_t k = bins_; k < fft_size_; ++k) {
        work_[k] = std::complex<double>(0.0, 0.0);
    }

    double* buffer = reinterpret_cast<double*>(work_.data());
    simd::dft(buffer, fft_size_);
    simd::complexMultiply(buffer, reinterpret_cast<const double*>(post_.data()), fft_size_);

    const double scale = 2.0 / static_cast<double>(bins_);
    for (size_t n = 0; n < fft_size_; ++n) {
        time[n] = static_cast<float>(scale * buffer[2 * n]);
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
