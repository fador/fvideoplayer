#include "audio/opus/SilkExcitation.hpp"

#include "audio/opus/SilkTables.hpp"

#include <algorithm>

namespace player::audio::silk {
namespace {

constexpr int kShellFrameLength = 16;
constexpr int kLog2ShellFrameLength = 4;
constexpr int kSilkMaxPulses = 16;
constexpr int kNbRateLevels = 10;
constexpr int kMaxNbShellBlocks = 20;  // MAX_FRAME_LENGTH / SHELL_CODEC_FRAME_LENGTH

// Splits the pulse count of a subframe between its two children
// (shell_coder.c decode_split).
void decodeSplit(int16_t* child1, int16_t* child2, opus::RangeDecoder& decoder, int p,
                 const uint8_t* shell_table) {
    if (p > 0) {
        *child1 = static_cast<int16_t>(decoder.decodeIcdf(&shell_table[kSilkShellCodeOffsets[p]], 8));
        *child2 = static_cast<int16_t>(p - *child1);
    } else {
        *child1 = 0;
        *child2 = 0;
    }
}

// Shell decoder for one block of 16 pulses (silk_shell_decoder).
void shellDecode(int16_t* pulses, opus::RangeDecoder& decoder, int pulses4) {
    int16_t pulses3[2];
    int16_t pulses2[4];
    int16_t pulses1[8];

    decodeSplit(&pulses3[0], &pulses3[1], decoder, pulses4, kSilkShellCodeTable3);

    decodeSplit(&pulses2[0], &pulses2[1], decoder, pulses3[0], kSilkShellCodeTable2);

    decodeSplit(&pulses1[0], &pulses1[1], decoder, pulses2[0], kSilkShellCodeTable1);
    decodeSplit(&pulses[0], &pulses[1], decoder, pulses1[0], kSilkShellCodeTable0);
    decodeSplit(&pulses[2], &pulses[3], decoder, pulses1[1], kSilkShellCodeTable0);

    decodeSplit(&pulses1[2], &pulses1[3], decoder, pulses2[1], kSilkShellCodeTable1);
    decodeSplit(&pulses[4], &pulses[5], decoder, pulses1[2], kSilkShellCodeTable0);
    decodeSplit(&pulses[6], &pulses[7], decoder, pulses1[3], kSilkShellCodeTable0);

    decodeSplit(&pulses2[2], &pulses2[3], decoder, pulses3[1], kSilkShellCodeTable2);

    decodeSplit(&pulses1[4], &pulses1[5], decoder, pulses2[2], kSilkShellCodeTable1);
    decodeSplit(&pulses[8], &pulses[9], decoder, pulses1[4], kSilkShellCodeTable0);
    decodeSplit(&pulses[10], &pulses[11], decoder, pulses1[5], kSilkShellCodeTable0);

    decodeSplit(&pulses1[6], &pulses1[7], decoder, pulses2[3], kSilkShellCodeTable1);
    decodeSplit(&pulses[12], &pulses[13], decoder, pulses1[6], kSilkShellCodeTable0);
    decodeSplit(&pulses[14], &pulses[15], decoder, pulses1[7], kSilkShellCodeTable0);
}

// Attaches signs to the non-zero pulses (silk_decode_signs).
void decodeSigns(opus::RangeDecoder& decoder, int16_t* pulses, int length, int signal_type,
                 int quant_offset_type, const int* sum_pulses) {
    uint8_t icdf[2];
    icdf[1] = 0;
    const uint8_t* icdf_ptr = &kSilkSignIcdf[7 * (quant_offset_type + 2 * signal_type)];
    const int blocks = (length + kShellFrameLength / 2) >> kLog2ShellFrameLength;

    int16_t* q = pulses;
    for (int b = 0; b < blocks; ++b) {
        const int p = sum_pulses[b];
        if (p > 0) {
            icdf[0] = icdf_ptr[std::min(p & 0x1F, 6)];
            for (int j = 0; j < kShellFrameLength; ++j) {
                if (q[j] > 0) {
                    q[j] = static_cast<int16_t>(q[j] * (2 * decoder.decodeIcdf(icdf, 8) - 1));
                }
            }
        }
        q += kShellFrameLength;
    }
}

}  // namespace

void decodePulses(opus::RangeDecoder& decoder, int16_t* pulses, int signal_type,
                  int quant_offset_type, int frame_length) {
    // Rate level controls the pulse count distribution.
    const int rate_level = decoder.decodeIcdf(&kSilkRateLevelsIcdf[(signal_type >> 1) * 9], 8);

    int iter = frame_length >> kLog2ShellFrameLength;
    if (iter * kShellFrameLength < frame_length) {
        ++iter;  // 10 ms at 12 kHz is 120 samples, not a multiple of 16
    }

    int sum_pulses[kMaxNbShellBlocks];
    int n_lshifts[kMaxNbShellBlocks];
    const uint8_t* cdf = &kSilkPulsesPerBlockIcdf[rate_level * 18];
    for (int i = 0; i < iter; ++i) {
        n_lshifts[i] = 0;
        sum_pulses[i] = decoder.decodeIcdf(cdf, 8);
        while (sum_pulses[i] == kSilkMaxPulses + 1) {
            ++n_lshifts[i];
            // After 10 LSBs the escape symbol is removed from the table.
            sum_pulses[i] = decoder.decodeIcdf(
                &kSilkPulsesPerBlockIcdf[(kNbRateLevels - 1) * 18 + (n_lshifts[i] == 10 ? 1 : 0)], 8);
        }
    }

    for (int i = 0; i < iter; ++i) {
        int16_t* block = pulses + i * kShellFrameLength;
        if (sum_pulses[i] > 0) {
            shellDecode(block, decoder, sum_pulses[i]);
        } else {
            std::fill(block, block + kShellFrameLength, static_cast<int16_t>(0));
        }
    }

    for (int i = 0; i < iter; ++i) {
        if (n_lshifts[i] > 0) {
            int16_t* block = pulses + i * kShellFrameLength;
            for (int k = 0; k < kShellFrameLength; ++k) {
                int abs_q = block[k];
                for (int j = 0; j < n_lshifts[i]; ++j) {
                    abs_q = (abs_q << 1) + decoder.decodeIcdf(kSilkLsbIcdf, 8);
                }
                block[k] = static_cast<int16_t>(abs_q);
            }
            sum_pulses[i] |= n_lshifts[i] << 5;
        }
    }

    decodeSigns(decoder, pulses, frame_length, signal_type, quant_offset_type, sum_pulses);
}

}  // namespace player::audio::silk
