#pragma once

#include "demux/Demuxer.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

namespace player::demux {

// Treats a raw Annex B .hevc/.h265 byte stream as a constant-frame-rate video.
// Access unit boundaries are derived from VCL NAL units and their
// first_slice_segment_in_pic_flag. Intended for decoder testing and for files
// that are not inside a container.
class RawHevcDemuxer final : public IDemuxer {
public:
    explicit RawHevcDemuxer(double fps = 25.0);
    ~RawHevcDemuxer() override;

    RawHevcDemuxer(const RawHevcDemuxer&) = delete;
    RawHevcDemuxer& operator=(const RawHevcDemuxer&) = delete;

    bool open(const std::filesystem::path& path, std::string& error) override;
    const VideoTrackInfo& video() const override { return video_; }
    const AudioTrackInfo& audio() const override { return audio_; }
    double duration() const override;
    bool readSample(Sample& sample) override;
    bool seek(double seconds) override;

    struct AccessUnit {
        int64_t offset = 0;
        int64_t size = 0;
        bool keyframe = false;
    };

private:
    FILE* file_ = nullptr;
    int64_t file_size_ = 0;
    double fps_ = 25.0;
    VideoTrackInfo video_;
    AudioTrackInfo audio_;
    std::vector<AccessUnit> units_;
    size_t next_unit_ = 0;
};

}  // namespace player::demux
