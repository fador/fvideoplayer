#include "audio/opus/CeltEnergy.hpp"

#include "audio/opus/OpusTables.hpp"

#include <algorithm>

namespace player::audio::opus {
namespace {

// Laplace distribution constants from RFC 6716 section 4.3.2.
constexpr unsigned kLaplaceLogMinP = 0;
constexpr unsigned kLaplaceMinP = 1u << kLaplaceLogMinP;
constexpr unsigned kLaplaceNMin = 16;

// Energy prediction coefficients (Q15 in the reference, plain floats here).
constexpr float kPredCoef[4] = {29440.0f / 32768.0f, 26112.0f / 32768.0f, 21248.0f / 32768.0f,
                                16384.0f / 32768.0f};
constexpr float kBetaCoef[4] = {30147.0f / 32768.0f, 22282.0f / 32768.0f, 12124.0f / 32768.0f,
                                6554.0f / 32768.0f};
constexpr float kBetaIntra = 4915.0f / 32768.0f;

unsigned laplaceFreq1(unsigned fs0, int decay) {
    const unsigned total = 32768 - kLaplaceMinP * (2 * kLaplaceNMin) - fs0;
    return static_cast<unsigned>((static_cast<int>(total) * (16384 - decay)) >> 15);
}

}  // namespace

int decodeLaplace(RangeDecoder& decoder, unsigned fs, int decay) {
    int value = 0;
    unsigned fl = 0;
    const unsigned fm = decoder.decodeBin(15);

    if (fm >= fs) {
        ++value;
        fl = fs;
        fs = laplaceFreq1(fs, decay) + kLaplaceMinP;
        while (fs > kLaplaceMinP && fm >= fl + 2 * fs) {
            fs *= 2;
            fl += fs;
            fs = static_cast<unsigned>((static_cast<int>(fs) - 2 * static_cast<int>(kLaplaceMinP)) *
                                       decay) >>
                 15;
            fs += kLaplaceMinP;
            ++value;
        }
        if (fs <= kLaplaceMinP) {
            const int di = static_cast<int>((fm - fl) >> (kLaplaceLogMinP + 1));
            value += di;
            fl += static_cast<unsigned>(2 * di * static_cast<int>(kLaplaceMinP));
        }
        if (fm < fl + fs) {
            value = -value;
        } else {
            fl += fs;
        }
    }
    decoder.update(fl, std::min(fl + fs, 32768u), 32768);
    return value;
}

void decodeCoarseEnergy(RangeDecoder& decoder, int start, int end, float* old_energy, bool intra,
                        int channels, int lm) {
    const uint8_t* model = kCeltEnergyProb + (lm * 2 + (intra ? 1 : 0)) * 42;
    const float coef = intra ? 0.0f : kPredCoef[lm];
    const float beta = intra ? kBetaIntra : kBetaCoef[lm];
    const int budget = static_cast<int>(decoder.size()) * 8;
    float prev[2] = {0.0f, 0.0f};

    for (int i = start; i < end; ++i) {
        for (int c = 0; c < channels; ++c) {
            const int tell = decoder.tell();
            int qi = 0;
            if (budget - tell >= 15) {
                const int pi = 2 * (i < 20 ? i : 20);
                qi = decodeLaplace(decoder, model[pi] << 7, model[pi + 1] << 6);
            } else if (budget - tell >= 2) {
                qi = decoder.decodeIcdf(kCeltSmallEnergyIcdf, 2);
                qi = (qi >> 1) ^ -(qi & 1);
            } else if (budget - tell >= 1) {
                qi = -static_cast<int>(decoder.decodeBitLogp(1));
            } else {
                qi = -1;
            }

            float* energy = &old_energy[i + c * kCeltBands];
            *energy = std::max(-9.0f, *energy);
            const float q = static_cast<float>(qi);
            *energy = coef * (*energy) + prev[c] + q;
            prev[c] = prev[c] + q - beta * q;
        }
    }
}

void decodeFineEnergy(RangeDecoder& decoder, int start, int end, float* old_energy,
                      const int* fine_quant, int channels) {
    for (int i = start; i < end; ++i) {
        if (fine_quant[i] <= 0) {
            continue;
        }
        for (int c = 0; c < channels; ++c) {
            const int q2 = static_cast<int>(decoder.decodeBits(static_cast<unsigned>(fine_quant[i])));
            const float offset = (static_cast<float>(q2) + 0.5f) *
                                     static_cast<float>(1 << (14 - fine_quant[i])) *
                                     (1.0f / 16384.0f) -
                                 0.5f;
            old_energy[i + c * kCeltBands] += offset;
        }
    }
}

void decodeFinalEnergy(RangeDecoder& decoder, int start, int end, float* old_energy,
                       const int* fine_quant, const int* fine_priority, int bits_left,
                       int channels) {
    for (int prio = 0; prio < 2; ++prio) {
        for (int i = start; i < end && bits_left >= channels; ++i) {
            if (fine_quant[i] >= kCeltMaxFineBits || fine_priority[i] != prio) {
                continue;
            }
            for (int c = 0; c < channels; ++c) {
                const int q2 = static_cast<int>(decoder.decodeBits(1));
                const float offset = (static_cast<float>(q2) - 0.5f) *
                                     static_cast<float>(1 << (14 - fine_quant[i] - 1)) *
                                     (1.0f / 16384.0f);
                old_energy[i + c * kCeltBands] += offset;
                --bits_left;
            }
        }
    }
}

}  // namespace player::audio::opus
