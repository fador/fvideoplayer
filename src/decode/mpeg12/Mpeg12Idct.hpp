#pragma once

#include <cstdint>

namespace player::decode::mpeg12 {

// 8x8 inverse DCT coefficients in natural (raster) order.
struct Block {
    int16_t coef[64];
};

// Clears a block.
inline void clearBlock(Block& block) {
    for (int i = 0; i < 64; ++i) {
        block.coef[i] = 0;
    }
}

// ISO/IEC 11172-2 clause 7.4.2 inverse quantisation. `quant_scale` is the
// quantiser scale and `matrix` is the quantisation matrix entry for the
// coefficient's scan position.
//
// MPEG-1 forces every reconstructed AC coefficient odd, which is what keeps
// its integer IDCT bit exact. MPEG-2 dropped that rule, so `force_odd` selects
// between them.
int inverseQuantize(int level, int quant_scale, int matrix, bool intra, bool force_odd = true);

// Transforms `block` and writes it over an existing prediction (inter blocks).
void idctAddBlock(const Block& block, uint8_t* dst, int stride);

// Transforms `block` and writes it as-is (intra blocks). The DC predictor is
// folded into the DC coefficient, so no level shift is added here.
void idctPutBlock(const Block& block, uint8_t* dst, int stride);

}  // namespace player::decode::mpeg12



