#pragma once

#include "decode/mpeg12/Mpeg12BitReader.hpp"
#include "decode/mpeg12/Mpeg12Tables.hpp"

namespace player::decode::mpeg12 {

// Matches a variable-length code table against the next bits, trying the
// longest codeword first. This is required because the MPEG-1 macroblock_type
// tables are not prefix codes (`001` is a prefix of `00100`). Returns the
// matched entry, or nullptr when no codeword matches.
const VlcEntry* matchVlc(BitReader& reader, const VlcEntry* table, size_t count,
                         unsigned max_bits = 16);

// Reads a VLC and returns its symbol, or -1 on a truncated / invalid stream.
int readVlc(BitReader& reader, const VlcEntry* table, size_t count,
            unsigned max_bits = 16);

// macroblock_address_increment, Table B-1. Loops over stuffing and escape
// entries. Returns false when the slice terminator was read; `symbol` then
// holds kMbAddrIncrEnd so the caller can tell it apart from a damaged stream.
bool readMbAddressIncrement(BitReader& reader, int& increment, bool& end_of_slice);

// coded_block_pattern, Table B-3. The symbol is the 6-bit pattern.
int readCodedBlockPattern(BitReader& reader);

// motion_vector, Table B-14. `predictor` is updated in place. `f_code` is
// forward_f_code / backward_f_code. MPEG-1 vectors are in half-sample units.
int readMotionVector(BitReader& reader, int predictor, int f_code);

// macroblock_type for a P or B picture. Returns -1 when no codeword matches.
int readMbTypeP(BitReader& reader);
int readMbTypeB(BitReader& reader);

// macroblock_type in an I picture: one bit, optionally followed by a
// quantiser_scale_code. Returns -1 for the reserved `00` codeword.
// `quantiser_type` is set when a quantiser_scale_code follows.
int readMbTypeI(BitReader& reader, bool& quantiser_type);

// DC coefficient difference of an intra block, Tables B-2 and B-5.
int readIntraDcDifference(BitReader& reader, int component);

// Which AC coefficient code table a block uses. MPEG-1 always uses Mpeg1;
// MPEG-2 uses Mpeg2 for intra blocks when intra_vlc_format is set and Mpeg1
// otherwise, including for every non-intra block.
enum class AcTable {
    Mpeg1,
    Mpeg2,
};

// One AC (run, level) pair of a block. `coefficient_index` is advanced past
// the run and points at the coefficient the level belongs to; it is -1 before
// the DC of a non-intra block. Returns false at end_of_block.
bool readAcCoefficient(BitReader& reader, AcTable table, int& coefficient_index, int& level);

}  // namespace player::decode::mpeg12



