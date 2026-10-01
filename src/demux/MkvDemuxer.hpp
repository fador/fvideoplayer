#pragma once

#include "demux/Demuxer.hpp"

#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace player::demux {

// Matroska/WebM demuxer for HEVC video tracks (V_MPEGH/ISO/HEVC).
// Blocks contain length-prefixed NAL units; the length size comes from the
// hvcC CodecPrivate. Audio track info is exposed for the dummy audio sink.
class MkvDemuxer final : public IDemuxer {
public:
    MkvDemuxer();
    ~MkvDemuxer() override;

    MkvDemuxer(const MkvDemuxer&) = delete;
    MkvDemuxer& operator=(const MkvDemuxer&) = delete;

    bool open(const std::filesystem::path& path, std::string& error) override;
    const VideoTrackInfo& video() const override { return video_; }
    const AudioTrackInfo& audio() const override { return audio_; }
    double duration() const override { return duration_; }
    bool readSample(Sample& sample) override;
    bool seek(double seconds) override;

    struct SampleInfo {
        uint64_t offset = 0;
        uint32_t size = 0;
        int64_t pts = 0;
        bool keyframe = false;
    };

private:
    FILE* file_ = nullptr;
    int64_t file_size_ = 0;
    VideoTrackInfo video_;
    AudioTrackInfo audio_;
    double duration_ = -1.0;
    std::vector<SampleInfo> samples_;
    std::vector<std::pair<int64_t, size_t>> keyframes_;  // pts -> sample index
    size_t next_sample_ = 0;
};

}  // namespace player::demux
