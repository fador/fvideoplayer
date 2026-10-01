#pragma once

#include "demux/Demuxer.hpp"

#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace player::demux {

// ISO Base Media File Format (MP4/MOV) demuxer for HEVC video tracks.
// Supports non-fragmented files with hvc1/hev1 sample entries. Audio tracks
// are detected for the dummy audio sink but their samples are not read yet.
class Mp4Demuxer final : public IDemuxer {
public:
    Mp4Demuxer();
    ~Mp4Demuxer() override;

    Mp4Demuxer(const Mp4Demuxer&) = delete;
    Mp4Demuxer& operator=(const Mp4Demuxer&) = delete;

    bool open(const std::filesystem::path& path, std::string& error) override;
    const VideoTrackInfo& video() const override { return video_; }
    const AudioTrackInfo& audio() const override { return audio_; }
    double duration() const override { return duration_; }
    bool readSample(Sample& sample) override;
    bool seek(double seconds) override;

    struct SampleInfo {
        uint64_t offset = 0;
        uint32_t size = 0;
        int64_t dts = 0;
        int64_t pts = 0;
        bool keyframe = false;
    };

private:
    FILE* file_ = nullptr;
    VideoTrackInfo video_;
    AudioTrackInfo audio_;
    double duration_ = -1.0;
    std::vector<SampleInfo> samples_;
    std::vector<std::pair<int64_t, size_t>> keyframes_;  // pts -> sample index
    size_t next_sample_ = 0;
};

}  // namespace player::demux
