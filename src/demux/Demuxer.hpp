#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace player::demux {

enum class Codec {
    Unknown,
    Hevc,
};

enum class TrackType {
    Video,
    Audio,
};

// Payload format of compressed samples.
struct SampleFormat {
    bool annexb = false;        // Annex B start codes in sample data
    uint8_t nal_length_size = 4;  // length prefix size when !annexb (1..4)
};

struct VideoTrackInfo {
    Codec codec = Codec::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    double fps = 0.0;          // nominal/average frame rate, 0 if unknown
    uint32_t timescale = 1000;  // ticks per second for Sample::pts
    SampleFormat format;
    std::vector<uint8_t> hvcc;  // HEVCDecoderConfigurationRecord, may be empty
};

struct AudioTrackInfo {
    bool present = false;
    uint32_t codec_fourcc = 0;
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    std::vector<uint8_t> extradata;
};

struct Sample {
    std::vector<uint8_t> data;
    int64_t pts = 0;  // presentation timestamp in video().timescale units
    int64_t dts = 0;  // decoding timestamp in video().timescale units
    bool keyframe = false;
    TrackType track = TrackType::Video;
};

class IDemuxer {
public:
    virtual ~IDemuxer() = default;

    virtual bool open(const std::filesystem::path& path, std::string& error) = 0;

    virtual const VideoTrackInfo& video() const = 0;
    virtual const AudioTrackInfo& audio() const = 0;

    // Duration in seconds, or -1 if unknown.
    virtual double duration() const = 0;

    // Next video sample in decode order. Returns false on EOF/error.
    virtual bool readSample(Sample& sample) = 0;

    // Audio packets are not decoded yet; the dummy audio sink consumes them
    // through this call. Returns false when there is no audio track.
    virtual bool readAudioSample(Sample& sample) {
        (void)sample;
        return false;
    }

    // Seek to the sync sample at or before the given time (seconds).
    virtual bool seek(double seconds) = 0;
};

// Picks a demuxer based on file magic (falls back to extension).
std::unique_ptr<IDemuxer> createDemuxer(const std::filesystem::path& path, std::string& error);

}  // namespace player::demux
