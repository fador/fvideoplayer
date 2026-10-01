#include "audio/opus/OpusHead.hpp"

#include <cmath>

namespace player::audio::opus {
namespace {

constexpr size_t kHeadSize = 19;
constexpr char kMagic[] = "OpusHead";

bool fail(std::string* error, const char* message) {
    if (error) {
        *error = message;
    }
    return false;
}

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
}

uint32_t readU32(const uint8_t* data) {
    return static_cast<uint32_t>(data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24));
}

}  // namespace

bool parseOpusHead(const uint8_t* data, size_t size, OpusHead& head, std::string* error) {
    if (!data || size < kHeadSize) {
        return fail(error, "short OpusHead");
    }
    for (size_t i = 0; i < 8; ++i) {
        if (data[i] != static_cast<uint8_t>(kMagic[i])) {
            return fail(error, "bad OpusHead magic");
        }
    }

    //    OpusHead layout (RFC 7845)
    //    +-------------------------------------------+
    //    | "OpusHead"          | 8 bytes             |
    //    | version             | 1                   |
    //    | channel count       | 1                   |
    //    | pre-skip            | 2 (little endian)   |
    //    | input sample rate   | 4 (little endian)   |
    //    | output gain (Q7.8)  | 2 (little endian)   |
    //    | mapping family      | 1                   |
    //    | stream/coupled/map  | mapping family != 0 |
    //    +-------------------------------------------+
    head.version = data[8];
    head.channels = data[9];
    head.pre_skip = readU16(data + 10);
    head.input_sample_rate = readU32(data + 12);
    head.output_gain = static_cast<int16_t>(readU16(data + 16));
    head.mapping_family = data[18];

    if (head.channels == 0) {
        return fail(error, "OpusHead has no channels");
    }
    if (head.mapping_family == 0) {
        if (head.channels > 2) {
            return fail(error, "OpusHead family 0 supports at most 2 channels");
        }
        head.stream_count = 1;
        head.coupled_count = head.channels - 1;
        for (uint8_t i = 0; i < head.channels; ++i) {
            head.channel_mapping[i] = i;
        }
        return true;
    }
    if (head.mapping_family != 1) {
        return fail(error, "unsupported Opus channel mapping family");
    }
    if (size < kHeadSize + 2 + head.channels) {
        return fail(error, "short OpusHead channel mapping");
    }
    head.stream_count = data[kHeadSize];
    head.coupled_count = data[kHeadSize + 1];
    if (head.stream_count == 0 || head.coupled_count > head.stream_count) {
        return fail(error, "invalid Opus stream counts");
    }
    for (uint8_t i = 0; i < head.channels; ++i) {
        const uint8_t mapping = data[kHeadSize + 2 + i];
        if (mapping != 255 && mapping >= head.stream_count + head.coupled_count) {
            return fail(error, "invalid Opus channel mapping");
        }
        head.channel_mapping[i] = mapping;
    }
    return true;
}

float outputGainFactor(int16_t output_gain) {
    // Q7.8 dB, e.g. 256 = +1 dB.
    return static_cast<float>(std::pow(10.0, static_cast<double>(output_gain) / 5120.0));
}

}  // namespace player::audio::opus
