#pragma once

#include <cstdint>

namespace player::audio::aac {

// Scalefactor band boundaries: count bands, offsets has count+1 entries.
// Band i covers spectral coefficients [offsets[i], offsets[i+1]).
struct ScaleFactorBands {
    const uint16_t* offsets = nullptr;
    uint8_t count = 0;
};

// One Huffman codeword and the quantized tuple it encodes. values is padded
// with zeros beyond the codebook dimension. Codewords are up to 19 bits.
struct HuffmanEntry {
    uint32_t code;
    uint8_t bits;
    int8_t values[4];
};

struct HuffmanCodebook {
    uint8_t dimension = 0;     // 1, 2 or 4 values per codeword
    uint8_t max_abs = 0;       // largest magnitude in the codebook
    bool signed_values = false;  // values carry signs; no sign bits in stream
    bool escape = false;         // max_abs marks an escape sequence
    const HuffmanEntry* entries = nullptr;
    uint16_t entry_count = 0;
};

// Codebooks: 0 = scalefactors, 1..11 = spectral data (ISO/IEC 14496-3).
const HuffmanCodebook& huffmanCodebook(unsigned index);

ScaleFactorBands longBands(uint8_t sample_rate_index);
ScaleFactorBands shortBands(uint8_t sample_rate_index);
uint8_t tnsMaxBandsLong(uint8_t sample_rate_index);
uint8_t tnsMaxBandsShort(uint8_t sample_rate_index);

// TNS reflection coefficients; index = 2 * coef_compress + coef_res.
const float* tnsCoefficients(unsigned index);

}  // namespace player::audio::aac
