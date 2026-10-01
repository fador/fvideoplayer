#pragma once

#include "audio/Pcm.hpp"
#include "audio/aac/AacConfig.hpp"
#include "audio/aac/AacFilterbank.hpp"
#include "audio/aac/AacIcs.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace player::audio::aac {

enum class Status {
    Ok,
    Unsupported,  // valid AAC, but a feature this decoder does not implement
    Invalid,      // corrupt or truncated input
};

// From-scratch AAC-LC decoder. Input is one access unit: either a raw
// raw_data_block as stored by MP4/Matroska, or a complete ADTS frame as used
// by raw/AVI streams. Output is interleaved float PCM.
//
// Supported: AAC-LC, 1..6 channels, M/S and intensity stereo, PNS, TNS,
// pulses, long/short/KBD windows. Not supported: SBR/PS, AAC Main/LTP/SSR,
// 960-sample frames, coupling elements and program config elements.
class Decoder {
public:
    // config is an AudioSpecificConfig. An empty config is accepted; ADTS
    // headers then define the stream on the first decode().
    bool configure(const uint8_t* config, size_t size, std::string& error);

    bool configured() const { return configured_; }
    const AudioSpecificConfig& config() const { return config_; }

    Status decode(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error);

    // Clears inter-frame state (overlap-add, PNS) but keeps the config; used
    // after seeking.
    void flush();

    void reset();

private:
    bool checkSupport(const AudioSpecificConfig& config, std::string& error) const;
    Status decodeBlock(const uint8_t* data, size_t size, PcmBuffer& pcm, std::string& error);
    bool decodeElement(BitReader& reader, uint8_t type, std::string& error);
    bool decodeSingle(BitReader& reader, size_t channel, std::string& error);
    bool decodePair(BitReader& reader, size_t first, std::string& error);
    bool skipDataStreamElement(BitReader& reader, std::string& error);
    bool skipFillElement(BitReader& reader, uint32_t count, std::string& error);
    void applyMidSide(size_t first);
    void applyIntensity(size_t first, bool ms_present);
    void assemblePcm(PcmBuffer& pcm);

    AudioSpecificConfig config_;
    bool configured_ = false;
    uint8_t sample_rate_index_ = 0;
    uint32_t random_state_ = 0x1F2E3D4C;
    int64_t pts_ = 0;
    size_t element_index_ = 0;
    size_t element_channels_ = 0;
    std::vector<ChannelData> channels_;
    std::vector<Filterbank> filterbanks_;
    std::vector<uint8_t> ms_mask_;
};

}  // namespace player::audio::aac
