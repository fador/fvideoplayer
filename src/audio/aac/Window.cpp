#include "audio/aac/Window.hpp"

#include <algorithm>
#include <cmath>

namespace player::audio::aac {
namespace {

constexpr double kPi = 3.14159265358979323846;

// Modified Bessel function of the first kind, order 0, via its power series.
double besselI0(double x) {
    const double half = x / 2.0;
    double term = 1.0;
    double sum = 1.0;
    for (int k = 1; k < 64; ++k) {
        term *= half / static_cast<double>(k);
        const double contribution = term * term;
        sum += contribution;
        if (contribution < 1e-12 * sum) {
            break;
        }
    }
    return sum;
}

}  // namespace

std::vector<float> sineWindow(size_t bins) {
    std::vector<float> window(2 * bins);
    for (size_t i = 0; i < window.size(); ++i) {
        const double angle = kPi * (static_cast<double>(i) + 0.5) /
                             static_cast<double>(2 * bins);
        window[i] = static_cast<float>(std::sin(angle));
    }
    return window;
}

std::vector<float> kbdWindow(size_t bins, float alpha) {
    // Kaiser-Bessel derived window (ISO/IEC 14496-3, 4.6.11.3):
    //
    //   W'[k] = I0(pi*alpha*sqrt(1 - ((k - N/4)/(N/4))^2)), k = 0..N/2
    //   w[i] = sqrt( sum_{k=0}^{i} W'[k] / sum_{k=0}^{N/2} W'[k] ), i < N/2
    //
    // N is the full window length (2 * bins), so the Kaiser sequence is
    // centered at N/4 = bins/2. The full window mirrors the half:
    // W[i] = w[i], W[2*bins-1-i] = w[i], giving W[i]^2 + W[i+bins]^2 = 1.
    std::vector<double> kaiser(bins + 1);
    const double quarter = static_cast<double>(bins) / 2.0;
    for (size_t k = 0; k <= bins; ++k) {
        const double t = (static_cast<double>(k) - quarter) / quarter;
        kaiser[k] =
            besselI0(kPi * static_cast<double>(alpha) * std::sqrt(std::max(0.0, 1.0 - t * t)));
    }

    double total = 0.0;
    for (const double value : kaiser) {
        total += value;
    }

    std::vector<float> window(2 * bins);
    double running = 0.0;
    for (size_t n = 0; n < bins; ++n) {
        running += kaiser[n];
        window[n] = static_cast<float>(std::sqrt(running / total));
    }
    for (size_t n = 0; n < bins; ++n) {
        window[2 * bins - 1 - n] = window[n];
    }
    return window;
}

}  // namespace player::audio::aac
