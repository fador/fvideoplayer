#pragma once

#include "audio/BitReader.hpp"
#include "audio/aac/AacTables.hpp"

#include <cstdint>
#include <vector>

namespace player::audio::aac {

// Binary-tree decoder for one AAC Huffman codebook, built from the standard
// codeword/length table.
class HuffmanTable {
public:
    void build(const HuffmanCodebook& codebook);

    // Reads one codeword and returns its quantized tuple.
    bool decode(BitReader& reader, int8_t values[4], unsigned& dimension) const;

    // Reads one codeword and returns the symbol index (codebook 0 semantics).
    bool decodeSymbol(BitReader& reader, uint8_t& symbol) const;

    bool valid() const { return codebook_ != nullptr; }

private:
    struct Node {
        int16_t child[2] = {-1, -1};
        int16_t symbol = -1;
    };

    const HuffmanCodebook* codebook_ = nullptr;
    std::vector<Node> nodes_;
};

// Shared table set, built on first use; one instance per codebook 0..11.
const HuffmanTable& huffmanTable(unsigned index);

}  // namespace player::audio::aac
