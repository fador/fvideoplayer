#pragma once

#include "demux/Demuxer.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace player::demux {

// Native FLAC demuxer. Parses the metadata blocks (STREAMINFO, SEEKTABLE),
// indexes the frames and exposes the stream as a single audio track with no
// video. One frame is one sample, as in every other container here.
class FlacDemuxer final : public IDemuxer {
public:
    FlacDemuxer();
    ~FlacDemuxer() override;

    FlacDemuxer(const FlacDemuxer&) = delete;
    FlacDemuxer& operator=(const FlacDemuxer&) = delete;

    bool open(const std::filesystem::path& path, std::string& error) override;
    const VideoTrackInfo& video() const override { return video_; }
    const AudioTrackInfo& audio() const override { return audio_; }
    double duration() const override { return duration_; }
    bool readSample(Sample& sample) override;
    bool seek(double seconds) override;

    std::vector<AudioTrackInfo> audioTracks() const override;
    bool selectAudioTrack(size_t index) override;
    bool readAudioSample(Sample& sample) override;

private:
    struct FrameInfo {
        int64_t offset = 0;
        uint32_t size = 0;
        int64_t pts_ms = 0;
    };
    struct SeekPoint {
        uint64_t first_sample = 0;
        uint64_t byte_offset = 0;
        uint32_t frame_samples = 0;
    };

    bool readAt(int64_t offset, void* data, size_t size);
    bool parseMetadata(std::string& error);
    bool indexFrames(std::string& error);

    FILE* file_ = nullptr;
    std::filesystem::path path_;
    int64_t file_size_ = 0;
    int64_t first_frame_offset_ = 0;
    uint32_t sample_rate_ = 0;
    uint32_t channels_ = 0;
    uint32_t bits_ = 0;
    uint32_t block_size_ = 0;
    uint64_t total_samples_ = 0;
    std::vector<SeekPoint> seektable_;
    std::vector<uint8_t> stream_info_;
    VideoTrackInfo video_;
    AudioTrackInfo audio_;
    double duration_ = 0.0;
    std::vector<FrameInfo> frames_;
    size_t next_audio_ = 0;
};

}  // namespace player::demux

