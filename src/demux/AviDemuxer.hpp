#pragma once

#include "demux/Demuxer.hpp"

#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace player::demux {

// RIFF AVI demuxer for HEVC video streams (VFW fourcc HEVC/H265/hvc1/hev1).
// HEVC in AVI is not standardized: the stream format (length-prefixed NALs or
// Annex B) is detected from the codec extradata and the first sample. The
// idx1 index is used when present, otherwise the movi list is scanned.
// Audio streams are read as well: each one keeps its own cursor, which seek()
// moves along with the video one.
class AviDemuxer final : public IDemuxer {
public:
    AviDemuxer();
    ~AviDemuxer() override;

    AviDemuxer(const AviDemuxer&) = delete;
    AviDemuxer& operator=(const AviDemuxer&) = delete;

    bool open(const std::filesystem::path& path, std::string& error) override;
    const VideoTrackInfo& video() const override { return video_; }
    const AudioTrackInfo& audio() const override { return audio_; }
    double duration() const override { return duration_; }
    bool readSample(Sample& sample) override;
    bool seek(double seconds) override;
    bool readAudioSample(Sample& sample) override;
    std::vector<AudioTrackInfo> audioTracks() const override { return audio_tracks_; }
    bool selectAudioTrack(size_t index) override;

    struct SampleInfo {
        int64_t offset = 0;
        uint32_t size = 0;
        int64_t pts = 0;
        bool keyframe = false;
    };

    struct TrackSamples {
        std::vector<SampleInfo> samples;
        size_t next = 0;
    };

private:
    FILE* file_ = nullptr;
    VideoTrackInfo video_;
    AudioTrackInfo audio_;
    double duration_ = -1.0;
    double fps_ = 0.0;
    std::vector<uint8_t> annexb_prefix_;  // parameter sets prepended to keyframes
    std::vector<SampleInfo> samples_;
    std::vector<std::pair<int64_t, size_t>> keyframes_;  // pts -> sample index
    size_t next_sample_ = 0;
    std::vector<AudioTrackInfo> audio_tracks_;
    std::vector<TrackSamples> audio_samples_;
    size_t selected_audio_ = 0;
};

}  // namespace player::demux
