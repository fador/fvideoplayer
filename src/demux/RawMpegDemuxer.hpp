#pragma once

#include "demux/Demuxer.hpp"

namespace player::demux {

// Raw MPEG-1 or MPEG-2 video elementary stream (.mpg, .mpeg, .m1v, .m2v).
//
// The file is scanned for start codes once at open, and pictures are cut at
// picture headers. Picture order is decode order, which is what the MPEG-1
// decoder expects; the decoder reorders for display.
class RawMpegDemuxer : public IDemuxer {
public:
    struct PictureRange {
        int64_t offset = 0;
        int64_t size = 0;
        int type = 0;  // picture_coding_type, 1 = I
    };

    // Frame rate used for sample timestamps. Zero means the decoder reports the
    // one from the sequence header, and 25 is assumed until then.
    explicit RawMpegDemuxer(double fps = 0.0);
    ~RawMpegDemuxer();

    bool open(const std::filesystem::path& path, std::string& error) override;
    const VideoTrackInfo& video() const override { return video_; }
    const AudioTrackInfo& audio() const override { return audio_; }
    double duration() const override;
    bool readSample(Sample& sample) override;
    bool seek(double seconds) override;

    std::vector<PictureRange> pictures_;
    FILE* file_ = nullptr;
    VideoTrackInfo video_{};
    AudioTrackInfo audio_{};
    double fps_ = 25.0;
    size_t next_picture_ = 0;
};

}  // namespace player::demux

