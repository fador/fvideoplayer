#include "audio/aac/AacIcs.hpp"

#include "audio/Simd.hpp"
#include "audio/aac/Huffman.hpp"

#include <algorithm>
#include <cmath>

namespace player::audio::aac {
namespace {

constexpr uint32_t kLcgMultiplier = 1664525u;
constexpr uint32_t kLcgIncrement = 1013904223u;
constexpr int kScaleFactorZero = 60;
constexpr int kNoiseOffset = 90;
constexpr int kNoisePre = 256;
constexpr unsigned kNoisePreBits = 9;

// Gain control modes: windows, first-window test flag, adjustment bit count.
const uint8_t kGainMode[4][3] = {
    {1, 0, 5},  // ONLY_LONG
    {2, 1, 2},  // LONG_START
    {8, 0, 2},  // EIGHT_SHORT
    {2, 1, 5},  // LONG_STOP
};

bool fail(std::string& error, const char* message) {
    error = message;
    return false;
}

bool parseSectionData(BitReader& reader, const IcsInfo& info, uint8_t* band_type,
                      std::string& error) {
    const bool short_window = info.sequence == WindowSequence::EightShort;
    const unsigned length_bits = short_window ? 3 : 5;
    const uint32_t escape = (1u << length_bits) - 1;

    for (unsigned g = 0; g < info.num_groups; ++g) {
        unsigned sfb = 0;
        while (sfb < info.max_sfb) {
            uint32_t codebook = 0;
            if (!reader.read(4, codebook)) {
                return fail(error, "truncated section data");
            }
            if (codebook == 12) {
                return fail(error, "invalid section codebook");
            }
            uint32_t length = 0;
            uint32_t increment = 0;
            do {
                if (!reader.read(length_bits, increment)) {
                    return fail(error, "truncated section length");
                }
                length += increment;
            } while (increment == escape);
            if (length == 0 || sfb + length > info.max_sfb) {
                return fail(error, "invalid section length");
            }
            for (uint32_t i = 0; i < length; ++i) {
                band_type[g * kSfbStride + sfb + i] = static_cast<uint8_t>(codebook);
            }
            sfb += length;
        }
    }
    return true;
}

bool parseScaleFactors(BitReader& reader, const IcsInfo& info, const uint8_t* band_type,
                       uint32_t global_gain, ChannelData& channel, std::string& error) {
    const HuffmanTable& table = huffmanTable(0);
    int offset_gain = static_cast<int>(global_gain);
    int offset_noise = static_cast<int>(global_gain) - kNoiseOffset;
    int offset_intensity = 0;
    bool first_noise = true;
    unsigned index = 0;

    for (unsigned g = 0; g < info.num_groups; ++g) {
        for (unsigned sfb = 0; sfb < info.max_sfb; ++sfb, ++index) {
            const uint8_t band = band_type[g * kSfbStride + sfb];
            if (band == static_cast<uint8_t>(BandType::Zero)) {
                continue;
            }

            //    scalefactor coding (ISO/IEC 14496-3, 4.6.3.2)
            //    normal band:   dpcm on global_gain, gain = 2^((sf-100)/4)
            //    noise band:    first = 9-bit PCM, then dpcm, energy = 2^(sf/4)
            //    intensity band: dpcm from 0, scale = 2^(-sf/4)
            if (band == static_cast<uint8_t>(BandType::Noise)) {
                int delta = 0;
                if (first_noise) {
                    uint32_t value = 0;
                    if (!reader.read(kNoisePreBits, value)) {
                        return fail(error, "truncated noise energy");
                    }
                    delta = static_cast<int>(value) - kNoisePre;
                    first_noise = false;
                } else {
                    uint8_t symbol = 0;
                    if (!table.decodeSymbol(reader, symbol)) {
                        return fail(error, "invalid noise scalefactor");
                    }
                    delta = static_cast<int>(symbol) - kScaleFactorZero;
                }
                offset_noise = std::clamp(offset_noise + delta, -100, 155);
                channel.noise_energy[index] = std::exp2(0.25f * static_cast<float>(offset_noise));
                continue;
            }

            if (band == static_cast<uint8_t>(BandType::Intensity) ||
                band == static_cast<uint8_t>(BandType::Intensity2)) {
                uint8_t symbol = 0;
                if (!table.decodeSymbol(reader, symbol)) {
                    return fail(error, "invalid intensity scalefactor");
                }
                offset_intensity =
                    std::clamp(offset_intensity + static_cast<int>(symbol) - kScaleFactorZero, -155, 100);
                channel.intensity_scale[index] = std::exp2(-0.25f * static_cast<float>(offset_intensity));
                continue;
            }

            uint8_t symbol = 0;
            if (!table.decodeSymbol(reader, symbol)) {
                return fail(error, "invalid scalefactor");
            }
            offset_gain += static_cast<int>(symbol) - kScaleFactorZero;
            if (offset_gain < 0 || offset_gain > 255) {
                return fail(error, "scalefactor out of range");
            }
            channel.gain[index] = std::exp2(0.25f * static_cast<float>(offset_gain - 100));
        }
    }
    return true;
}

bool parsePulse(BitReader& reader, const IcsInfo& info, PulseData& pulse, std::string& error) {
    uint32_t count = 0;
    uint32_t start_sfb = 0;
    if (!reader.read(2, count) || !reader.read(6, start_sfb)) {
        return fail(error, "truncated pulse data");
    }
    if (start_sfb >= info.bands.count) {
        return fail(error, "pulse start band out of range");
    }
    pulse.count = static_cast<uint8_t>(count + 1);

    const uint16_t* offsets = info.bands.offsets;
    const uint32_t total = offsets[info.bands.count];
    uint32_t offset = 0;
    if (!reader.read(5, offset)) {
        return fail(error, "truncated pulse data");
    }
    uint32_t position = offsets[start_sfb] + offset;

    for (unsigned i = 0; i < pulse.count; ++i) {
        if (i > 0) {
            if (!reader.read(5, offset)) {
                return fail(error, "truncated pulse data");
            }
            position += offset;
        }
        if (position >= total) {
            return fail(error, "pulse position out of range");
        }
        uint32_t amplitude = 0;
        if (!reader.read(4, amplitude)) {
            return fail(error, "truncated pulse amplitude");
        }
        pulse.position[i] = static_cast<uint16_t>(position);
        pulse.amplitude[i] = static_cast<uint8_t>(amplitude);
    }
    return true;
}

bool parseTns(BitReader& reader, const IcsInfo& info, TnsData& tns, std::string& error) {
    const bool short_window = info.sequence == WindowSequence::EightShort;
    const unsigned max_order = short_window ? 7 : kMaxTnsOrder;

    for (unsigned w = 0; w < info.num_windows; ++w) {
        uint32_t filters = 0;
        if (!reader.read(short_window ? 1 : 2, filters)) {
            return fail(error, "truncated TNS data");
        }
        tns.n_filters[w] = static_cast<uint8_t>(filters);
        if (filters == 0) {
            continue;
        }
        uint32_t coef_res = 0;
        if (!reader.read(1, coef_res)) {
            return fail(error, "truncated TNS data");
        }
        for (unsigned f = 0; f < filters; ++f) {
            TnsFilter& filter = tns.filters[w][f];
            uint32_t length = 0;
            uint32_t order = 0;
            if (!reader.read(short_window ? 4 : 6, length) ||
                !reader.read(short_window ? 3 : 5, order)) {
                return fail(error, "truncated TNS filter");
            }
            if (order > max_order) {
                return fail(error, "TNS order too large");
            }
            filter.length = static_cast<uint8_t>(length);
            filter.order = static_cast<uint8_t>(order);
            if (order == 0) {
                continue;
            }
            uint32_t direction = 0;
            uint32_t compress = 0;
            if (!reader.read(1, direction) || !reader.read(1, compress)) {
                return fail(error, "truncated TNS filter");
            }
            filter.direction = direction != 0;
            const unsigned coefficient_bits = coef_res + 3 - compress;
            const float* table = tnsCoefficients(2 * compress + coef_res);
            if (!table) {
                return fail(error, "missing TNS coefficient table");
            }
            for (unsigned i = 0; i < order; ++i) {
                uint32_t value = 0;
                if (!reader.read(coefficient_bits, value)) {
                    return fail(error, "truncated TNS coefficients");
                }
                filter.reflection[i] = table[value];
            }
        }
    }
    return true;
}

bool skipGainControl(BitReader& reader, const IcsInfo& info, std::string& error) {
    uint32_t max_band = 0;
    if (!reader.read(2, max_band)) {
        return fail(error, "truncated gain control");
    }
    const uint8_t* mode = kGainMode[static_cast<unsigned>(info.sequence)];
    for (uint32_t band = 0; band < max_band; ++band) {
        for (unsigned wd = 0; wd < mode[0]; ++wd) {
            uint32_t adjustments = 0;
            if (!reader.read(3, adjustments)) {
                return fail(error, "truncated gain control");
            }
            const unsigned bits = 4 + ((wd == 0 && mode[1]) ? 4 : mode[2]);
            for (uint32_t i = 0; i < adjustments; ++i) {
                if (!reader.skip(bits)) {
                    return fail(error, "truncated gain control");
                }
            }
        }
    }
    return true;
}

bool readEscape(BitReader& reader, int32_t& magnitude, std::string& error) {
    unsigned leading = 0;
    for (;;) {
        uint32_t bit = 0;
        if (!reader.read(1, bit)) {
            return fail(error, "truncated escape sequence");
        }
        if (bit == 0) {
            break;
        }
        if (++leading > 8) {
            return fail(error, "escape sequence overflow");
        }
    }
    const unsigned width = leading + 4;
    uint32_t rest = 0;
    if (!reader.read(width, rest)) {
        return fail(error, "truncated escape sequence");
    }
    magnitude = static_cast<int32_t>((1u << width) | rest);
    return true;
}

bool decodeBand(BitReader& reader, uint8_t codebook, unsigned width, int16_t* out,
                std::string& error) {
    const HuffmanTable& table = huffmanTable(codebook);
    const HuffmanCodebook& spec = huffmanCodebook(codebook);
    unsigned position = 0;

    while (position < width) {
        int8_t values[4] = {};
        unsigned dimension = 0;
        if (!table.decode(reader, values, dimension)) {
            return fail(error, "invalid spectral code");
        }
        if (position + dimension > width) {
            return fail(error, "spectral band overflow");
        }

        // Unsigned codebooks transmit one sign bit per non-zero value.
        bool negative[4] = {};
        if (!spec.signed_values) {
            for (unsigned i = 0; i < dimension; ++i) {
                if (values[i] == 0) {
                    continue;
                }
                uint32_t sign = 0;
                if (!reader.read(1, sign)) {
                    return fail(error, "truncated spectral sign");
                }
                negative[i] = sign != 0;
            }
        }

        for (unsigned i = 0; i < dimension; ++i) {
            int32_t value = values[i];
            if (spec.escape && value == spec.max_abs) {
                if (!readEscape(reader, value, error)) {
                    return false;
                }
            }
            if (negative[i]) {
                value = -value;
            }
            out[position + i] = static_cast<int16_t>(value);
        }
        position += dimension;
    }
    return true;
}

void generateNoise(uint32_t& state, float* out, unsigned width, float energy) {
    float sum = 0.0f;
    for (unsigned i = 0; i < width; ++i) {
        state = state * kLcgMultiplier + kLcgIncrement;
        out[i] = static_cast<float>(static_cast<int32_t>(state));
        sum += out[i] * out[i];
    }
    if (sum <= 0.0f) {
        return;
    }
    const float scale = energy / std::sqrt(sum);
    for (unsigned i = 0; i < width; ++i) {
        out[i] *= scale;
    }
}

bool decodeSpectralData(BitReader& reader, const IcsInfo& info, ChannelData& channel,
                        uint32_t& random_state, std::string& error) {
    const uint16_t* offsets = info.bands.offsets;

    // Coefficients above max_sfb stay zero. Long windows own 1024
    // coefficients, each short window 128.
    const unsigned window_length =
        info.sequence == WindowSequence::EightShort ? kWindowSize : kFrameSamples;
    for (unsigned w = 0; w < info.num_windows; ++w) {
        float* spectrum = channel.spectrum + w * window_length;
        int16_t* quant = channel.quant + w * window_length;
        std::fill(spectrum + offsets[info.max_sfb], spectrum + window_length, 0.0f);
        std::fill(quant + offsets[info.max_sfb], quant + window_length, static_cast<int16_t>(0));
    }

    unsigned window_index = 0;
    unsigned band_index = 0;
    for (unsigned g = 0; g < info.num_groups; ++g) {
        const unsigned group_length = info.group_length[g];
        for (unsigned sfb = 0; sfb < info.max_sfb; ++sfb, ++band_index) {
            const uint8_t band = channel.band_type[g * kSfbStride + sfb];
            const unsigned width = offsets[sfb + 1] - offsets[sfb];
            for (unsigned j = 0; j < group_length; ++j) {
                const unsigned base = (window_index + j) * kWindowSize + offsets[sfb];
                if (band == static_cast<uint8_t>(BandType::Zero)) {
                    continue;
                }
                if (band == static_cast<uint8_t>(BandType::Noise)) {
                    generateNoise(random_state, channel.spectrum + base, width,
                                  channel.noise_energy[band_index]);
                    continue;
                }
                if (band >= static_cast<uint8_t>(BandType::Intensity)) {
                    std::fill(channel.spectrum + base, channel.spectrum + base + width, 0.0f);
                    continue;
                }
                if (!decodeBand(reader, band, width, channel.quant + base, error)) {
                    return false;
                }
            }
        }
        window_index += group_length;
    }
    return true;
}

void applyPulses(ChannelData& channel, const PulseData& pulse) {
    if (pulse.count == 0) {
        return;
    }
    const IcsInfo& info = channel.info;
    const uint16_t* offsets = info.bands.offsets;
    unsigned sfb = 0;

    for (unsigned i = 0; i < pulse.count; ++i) {
        const uint32_t position = pulse.position[i];
        while (sfb + 1 < info.bands.count && offsets[sfb + 1] <= position) {
            ++sfb;
        }
        const uint8_t band = channel.band_type[sfb];
        if (band == static_cast<uint8_t>(BandType::Zero) || band >= 13) {
            continue;
        }
        if (channel.gain[sfb] == 0.0f) {
            continue;
        }
        const int16_t amplitude = static_cast<int16_t>(1u << pulse.amplitude[i]);
        int16_t& value = channel.quant[position];
        value = static_cast<int16_t>(value > 0 ? value + amplitude : value - amplitude);
    }
}

void dequantize(ChannelData& channel) {
    const IcsInfo& info = channel.info;
    const uint16_t* offsets = info.bands.offsets;
    unsigned window_index = 0;
    unsigned band_index = 0;

    for (unsigned g = 0; g < info.num_groups; ++g) {
        const unsigned group_length = info.group_length[g];
        for (unsigned sfb = 0; sfb < info.max_sfb; ++sfb, ++band_index) {
            const uint8_t band = channel.band_type[g * kSfbStride + sfb];
            if (band == static_cast<uint8_t>(BandType::Zero) ||
                band >= static_cast<uint8_t>(BandType::Noise)) {
                continue;
            }
            const float gain = channel.gain[band_index];
            const unsigned width = offsets[sfb + 1] - offsets[sfb];
            for (unsigned j = 0; j < group_length; ++j) {
                const unsigned base = (window_index + j) * kWindowSize + offsets[sfb];
                simd::dequantizeBand(channel.quant + base, channel.spectrum + base, width, gain);
            }
        }
        window_index += group_length;
    }
}

// Reflection coefficients to LPC via the Levinson-Durbin recursion.
void computeLpc(const float* reflection, unsigned order, float* lpc) {
    for (unsigned i = 0; i < order; ++i) {
        lpc[i] = 0.0f;
    }
    for (unsigned i = 0; i < order; ++i) {
        const float r = reflection[i];
        unsigned j = 0;
        for (; j < (i + 1) / 2; ++j) {
            const float tmp = lpc[j];
            lpc[j] += r * lpc[i - 1 - j];
            lpc[i - 1 - j] += r * tmp;
        }
        if ((i & 1) != 0) {
            lpc[j] += lpc[j] * r;
        }
        lpc[i] = -r;
    }
}

}  // namespace

void ChannelData::reset() {
    std::fill(std::begin(band_type), std::end(band_type), static_cast<uint8_t>(0));
    std::fill(std::begin(gain), std::end(gain), 0.0f);
    std::fill(std::begin(noise_energy), std::end(noise_energy), 0.0f);
    std::fill(std::begin(intensity_scale), std::end(intensity_scale), 0.0f);
    std::fill(std::begin(quant), std::end(quant), static_cast<int16_t>(0));
    std::fill(std::begin(spectrum), std::end(spectrum), 0.0f);
    tns = TnsData{};
    tns_present = false;
}

bool parseIcsInfo(BitReader& reader, uint8_t sample_rate_index, IcsInfo& info,
                  std::string* error) {
    uint32_t reserved = 0;
    uint32_t sequence = 0;
    uint32_t shape = 0;
    if (!reader.read(1, reserved) || !reader.read(2, sequence) || !reader.read(1, shape)) {
        if (error) {
            *error = "truncated ICS info";
        }
        return false;
    }
    info.sequence = static_cast<WindowSequence>(sequence);
    info.kbd_window = shape != 0;

    if (info.sequence == WindowSequence::EightShort) {
        uint32_t max_sfb = 0;
        uint32_t grouping = 0;
        if (!reader.read(4, max_sfb) || !reader.read(7, grouping)) {
            if (error) {
                *error = "truncated short window info";
            }
            return false;
        }
        info.max_sfb = static_cast<uint8_t>(max_sfb);
        info.num_windows = 8;
        info.num_groups = 1;
        info.group_length[0] = 1;
        for (unsigned i = 1; i < 8; ++i) {
            info.group_length[i] = 0;
        }
        for (unsigned i = 0; i < 7; ++i) {
            if ((grouping >> (6 - i)) & 1u) {
                ++info.group_length[info.num_groups - 1];
            } else {
                ++info.num_groups;
                info.group_length[info.num_groups - 1] = 1;
            }
        }
        info.bands = shortBands(sample_rate_index);
        info.tns_max_bands = tnsMaxBandsShort(sample_rate_index);
    } else {
        uint32_t max_sfb = 0;
        uint32_t predictor = 0;
        if (!reader.read(6, max_sfb) || !reader.read(1, predictor)) {
            if (error) {
                *error = "truncated long window info";
            }
            return false;
        }
        if (predictor != 0) {
            if (error) {
                *error = "prediction/LTP is not supported in AAC-LC";
            }
            return false;
        }
        info.max_sfb = static_cast<uint8_t>(max_sfb);
        info.num_windows = 1;
        info.num_groups = 1;
        info.group_length[0] = 1;
        for (unsigned i = 1; i < 8; ++i) {
            info.group_length[i] = 0;
        }
        info.bands = longBands(sample_rate_index);
        info.tns_max_bands = tnsMaxBandsLong(sample_rate_index);
    }

    if (!info.bands.offsets || info.max_sfb > info.bands.count) {
        if (error) {
            *error = "max_sfb exceeds the scalefactor band table";
        }
        return false;
    }
    return true;
}

bool decodeIcsPayload(BitReader& reader, uint8_t sample_rate_index, ChannelData& channel,
                      const IcsInfo* shared_info, uint32_t& random_state, std::string& error) {
    uint32_t global_gain = 0;
    if (!reader.read(8, global_gain)) {
        return fail(error, "truncated global gain");
    }
    if (shared_info) {
        channel.info = *shared_info;
    } else if (!parseIcsInfo(reader, sample_rate_index, channel.info, &error)) {
        return false;
    }
    channel.reset();

    if (!parseSectionData(reader, channel.info, channel.band_type, error)) {
        return false;
    }
    if (!parseScaleFactors(reader, channel.info, channel.band_type, global_gain, channel, error)) {
        return false;
    }

    uint32_t flag = 0;
    if (!reader.read(1, flag)) {
        return fail(error, "truncated ICS");
    }
    PulseData pulse;
    if (flag != 0) {
        if (channel.info.sequence == WindowSequence::EightShort) {
            return fail(error, "pulse tool not allowed in short windows");
        }
        if (!parsePulse(reader, channel.info, pulse, error)) {
            return false;
        }
    }

    if (!reader.read(1, flag)) {
        return fail(error, "truncated ICS");
    }
    channel.tns_present = flag != 0;
    if (channel.tns_present && !parseTns(reader, channel.info, channel.tns, error)) {
        return false;
    }

    if (!reader.read(1, flag)) {
        return fail(error, "truncated ICS");
    }
    if (flag != 0 && !skipGainControl(reader, channel.info, error)) {
        return false;
    }

    if (!decodeSpectralData(reader, channel.info, channel, random_state, error)) {
        return false;
    }
    applyPulses(channel, pulse);
    dequantize(channel);
    return true;
}

void applyTns(const IcsInfo& info, const TnsData& tns, float* spectrum) {
    const unsigned limit = std::min<unsigned>(info.tns_max_bands, info.max_sfb);
    if (limit == 0 || !info.bands.offsets) {
        return;
    }
    const uint16_t* offsets = info.bands.offsets;

    for (unsigned w = 0; w < info.num_windows; ++w) {
        unsigned bottom = info.bands.count;
        for (unsigned f = 0; f < tns.n_filters[w]; ++f) {
            const TnsFilter& filter = tns.filters[w][f];
            const unsigned top = bottom;
            bottom = top > filter.length ? top - filter.length : 0;
            if (filter.order == 0) {
                continue;
            }

            float lpc[kMaxTnsOrder] = {};
            computeLpc(filter.reflection, filter.order, lpc);

            const int start = offsets[std::min(bottom, limit)];
            const int end = offsets[std::min(top, limit)];
            const int size = end - start;
            if (size <= 0) {
                continue;
            }

            const int increment = filter.direction ? -1 : 1;
            int position =
                (filter.direction ? end - 1 : start) + static_cast<int>(w) * kWindowSize;
            for (int m = 0; m < size; ++m, position += increment) {
                const unsigned order =
                    std::min<unsigned>(static_cast<unsigned>(m), filter.order);
                for (unsigned i = 1; i <= order; ++i) {
                    spectrum[position] -=
                        spectrum[position - static_cast<int>(i) * increment] * lpc[i - 1];
                }
            }
        }
    }
}

}  // namespace player::audio::aac
