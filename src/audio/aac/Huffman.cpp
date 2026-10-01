#include "audio/aac/Huffman.hpp"

#include <array>

namespace player::audio::aac {

void HuffmanTable::build(const HuffmanCodebook& codebook) {
    codebook_ = &codebook;
    nodes_.clear();
    if (!codebook.entries) {
        codebook_ = nullptr;
        return;
    }
    nodes_.reserve(static_cast<size_t>(codebook.entry_count) * 8);
    nodes_.emplace_back();

    for (uint16_t symbol = 0; symbol < codebook.entry_count; ++symbol) {
        const HuffmanEntry& entry = codebook.entries[symbol];
        int16_t node = 0;
        for (int bit_index = entry.bits - 1; bit_index >= 0; --bit_index) {
            const int bit = (entry.code >> bit_index) & 1;
            Node& current = nodes_[static_cast<size_t>(node)];
            if (current.child[bit] < 0) {
                current.child[bit] = static_cast<int16_t>(nodes_.size());
                Node next;
                if (bit_index == 0) {
                    next.symbol = static_cast<int16_t>(symbol);
                }
                nodes_.push_back(next);
            }
            node = current.child[bit];
        }
    }
}

bool HuffmanTable::decode(BitReader& reader, int8_t values[4], unsigned& dimension) const {
    if (!codebook_) {
        return false;
    }
    int16_t node = 0;
    for (;;) {
        const Node& current = nodes_[static_cast<size_t>(node)];
        if (current.symbol >= 0) {
            const HuffmanEntry& entry = codebook_->entries[current.symbol];
            for (unsigned i = 0; i < 4; ++i) {
                values[i] = entry.values[i];
            }
            dimension = codebook_->dimension;
            return true;
        }
        uint32_t bit = 0;
        if (!reader.read(1, bit)) {
            return false;
        }
        node = current.child[bit];
        if (node < 0) {
            return false;
        }
    }
}

bool HuffmanTable::decodeSymbol(BitReader& reader, uint8_t& symbol) const {
    int8_t values[4] = {};
    unsigned dimension = 0;
    if (!decode(reader, values, dimension) || dimension != 1) {
        return false;
    }
    symbol = static_cast<uint8_t>(values[0]);
    return true;
}

const HuffmanTable& huffmanTable(unsigned index) {
    static const std::array<HuffmanTable, 12> tables = [] {
        std::array<HuffmanTable, 12> built;
        for (unsigned i = 0; i < 12; ++i) {
            built[i].build(huffmanCodebook(i));
        }
        return built;
    }();
    static const HuffmanTable empty;
    return index < tables.size() ? tables[index] : empty;
}

}  // namespace player::audio::aac
