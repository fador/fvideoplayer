#include "decode/mpeg12/Mpeg12Idct.hpp"

#include <cmath>

namespace player::decode::mpeg12 {
namespace {

// c(u) * cos((2x + 1) * u * pi / 16) with c(0) = 1/sqrt(2), c(u) = 1 otherwise.
// One row per output position x, one column per input coefficient u.
struct Basis {
    float value[8][8];
};

const Basis& basis() {
    static const Basis table = [] {
        Basis result{};
        constexpr float kInvSqrt2 = 0.70710678118654752f;
        for (int x = 0; x < 8; ++x) {
            for (int u = 0; u < 8; ++u) {
                const float scale = u == 0 ? kInvSqrt2 : 1.0f;
                result.value[x][u] =
                    static_cast<float>(scale *
                                       std::cos((2.0 * x + 1.0) * u * 3.14159265358979323846 / 16.0));
            }
        }
        return result;
    }();
    return table;
}

// One 8-point pass of the separable inverse transform. `basis` is the whole
// matrix: row n is the basis for output position n, so every output uses its
// own row.
void transform1d(const float* in, float* out, const Basis& basis) {
    for (int n = 0; n < 8; ++n) {
        float sum = 0.0f;
        for (int k = 0; k < 8; ++k) {
            sum += basis.value[n][k] * in[k];
        }
        out[n] = sum * 0.5f;
    }
}

void idct8x8(const int16_t* coef, float out[64]) {
    const Basis& b = basis();
    float rows[64];

    // Vertical pass: input is indexed by vertical frequency, output by row.
    for (int x = 0; x < 8; ++x) {
        float column[8];
        for (int u = 0; u < 8; ++u) {
            column[u] = static_cast<float>(coef[u * 8 + x]);
        }
        float result[8];
        transform1d(column, result, b);
        for (int y = 0; y < 8; ++y) {
            rows[y * 8 + x] = result[y];
        }
    }

    // Horizontal pass: input is indexed by horizontal frequency, output by
    // column.
    for (int y = 0; y < 8; ++y) {
        transform1d(rows + y * 8, out + y * 8, b);
    }
}

inline uint8_t clampToByte(int value) {
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

inline int roundNearest(float value) {
    return static_cast<int>(std::lround(value));
}

}  // namespace

int inverseQuantize(int level, int quant_scale, int matrix, bool intra, bool force_odd) {
    const int magnitude = level < 0 ? -level : level;
    int reconstructed;
    if (intra) {
        reconstructed = (magnitude * quant_scale * matrix) >> 4;
    } else {
        reconstructed = ((magnitude * 2 + 1) * quant_scale * matrix) >> 5;
    }
    if (force_odd) {
        // Every reconstructed MPEG-1 coefficient is forced odd: an even value
        // is rounded down by one.
        reconstructed = (reconstructed - 1) | 1;
    }
    return level < 0 ? -reconstructed : reconstructed;
}

void idctAddBlock(const Block& block, uint8_t* dst, int stride) {
    float pixels[64];
    idct8x8(block.coef, pixels);
    for (int y = 0; y < 8; ++y) {
        uint8_t* row = dst + static_cast<size_t>(y) * stride;
        for (int x = 0; x < 8; ++x) {
            row[x] = clampToByte(row[x] + roundNearest(pixels[y * 8 + x]));
        }
    }
}

void idctPutBlock(const Block& block, uint8_t* dst, int stride) {
    float pixels[64];
    idct8x8(block.coef, pixels);
    for (int y = 0; y < 8; ++y) {
        uint8_t* row = dst + static_cast<size_t>(y) * stride;
        for (int x = 0; x < 8; ++x) {
            row[x] = clampToByte(roundNearest(pixels[y * 8 + x]));
        }
    }
}

}  // namespace player::decode::mpeg12



