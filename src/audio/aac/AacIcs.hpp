#pragma once

#include "audio/BitReader.hpp"
#include "audio/aac/AacTables.hpp"

#include <cstdint>
#include <string>

namespace player::audio::aac {

enum class WindowSequence : uint8_t {
    OnlyLong = 0,
    LongStart = 1,
    EightShort = 2,
    LongStop = 3,
};

// Section codebook. Spectral codebooks 1..11 are used as their value.
enum class BandType : uint8_t {
    Zero = 0,
    Noise = 13,
    Intensity = 14,
    Intensity2 = 15,
};

constexpr unsigned kMaxWindows = 8;
constexpr unsigned kMaxSfb = 64;
constexpr unsigned kSfbStride = 64;
constexpr unsigned kWindowSize = 128;
constexpr unsigned kFrameSamples = 1024;
constexpr unsigned kMaxTnsOrder = 12;

// individual_channel_stream information (window sequence and scalefactor bands).
struct IcsInfo {
    WindowSequence sequence = WindowSequence::OnlyLong;
    bool kbd_window = false;
    uint8_t max_sfb = 0;
    uint8_t num_windows = 1;
    uint8_t num_groups = 1;
    uint8_t group_length[kMaxWindows] = {1, 0, 0, 0, 0, 0, 0, 0};
    ScaleFactorBands bands;
    uint8_t tns_max_bands = 0;
};

struct PulseData {
    uint8_t count = 0;
    uint16_t position[4] = {};
    uint8_t amplitude[4] = {};
};

struct TnsFilter {
    uint8_t length = 0;
    uint8_t order = 0;
    bool direction = false;
    float reflection[kMaxTnsOrder] = {};
};

struct TnsData {
    uint8_t n_filters[kMaxWindows] = {};
    TnsFilter filters[kMaxWindows][4];
};

// One channel's spectral data for a frame. Short windows are laid out
// consecutively: window w owns spectrum[w * 128 .. w * 128 + 128).
struct ChannelData {
    IcsInfo info;
    uint8_t band_type[kMaxWindows * kSfbStride] = {};
    float gain[kMaxWindows * kSfbStride] = {};
    float noise_energy[kMaxWindows * kSfbStride] = {};
    float intensity_scale[kMaxWindows * kSfbStride] = {};
    int16_t quant[kFrameSamples] = {};
    float spectrum[kFrameSamples] = {};
    TnsData tns;
    bool tns_present = false;

    void reset();
};

bool parseIcsInfo(BitReader& reader, uint8_t sample_rate_index, IcsInfo& info, std::string* error);

// Parses global_gain through spectral data into channel. shared_info is the
// CPE common window ICS; pass nullptr for standalone channels.
bool decodeIcsPayload(BitReader& reader, uint8_t sample_rate_index, ChannelData& channel,
                      const IcsInfo* shared_info, uint32_t& random_state, std::string& error);

void applyTns(const IcsInfo& info, const TnsData& tns, float* spectrum);

}  // namespace player::audio::aac
