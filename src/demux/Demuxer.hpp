#pragma once

#include "subtitle/Subtitle.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace player::demux {

enum class Codec {
    Unknown,
    Hevc,
    Mpeg1,
    Mpeg2,
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
    size_t index = 0;
    std::string name;
    std::string language;
    std::string codec;  // container codec id, e.g. "A_AAC", "mp4a"
    bool present = false;
    bool default_track = false;
    uint32_t codec_fourcc = 0;
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    std::vector<uint8_t> extradata;
};

struct SubtitleTrackInfo {
    size_t index = 0;
    std::string name;
    std::string language;
    std::string codec;  // "S_TEXT/UTF8", "S_TEXT/ASS", "tx3g", ...
    bool bitmap = false;
    std::vector<uint8_t> codec_private;
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

    // All audio tracks in the container (defaults to the single audio()).
    virtual std::vector<AudioTrackInfo> audioTracks() const {
        std::vector<AudioTrackInfo> tracks;
        if (audio().present) {
            tracks.push_back(audio());
        }
        return tracks;
    }

    // Selects the audio track whose packets readAudioSample returns.
    virtual bool selectAudioTrack(size_t index) {
        (void)index;
        return false;
    }

    // Text subtitle tracks in the container.
    virtual std::vector<SubtitleTrackInfo> subtitleTracks() const { return {}; }

    // Reads all blocks of a text subtitle track. Implementations that read
    // from disk must not disturb the main sample reader (use a private file
    // handle), because this may be called from the UI thread.
    virtual bool readSubtitleTrack(size_t index, std::vector<subtitle::SubtitleBlock>& blocks,
                                   std::string& error) {
        (void)index;
        (void)blocks;
        error = "subtitle extraction not supported";
        return false;
    }

    // Seek to the sync sample at or before the given time (seconds).
    virtual bool seek(double seconds) = 0;
};

// Picks a demuxer based on file magic (falls back to extension).
std::unique_ptr<IDemuxer> createDemuxer(const std::filesystem::path& path, std::string& error);

}  // namespace player::demux
