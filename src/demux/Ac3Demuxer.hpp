#pragma once

#include "demux/Demuxer.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace player::demux {

// Raw AC-3 (Dolby Digital) demuxer. Walks the syncframes at open time, so the
// frame index, the duration and the format are known before playback and a
// seek can land on any frame.
class Ac3Demuxer final : public IDemuxer {
public:
    Ac3Demuxer();
    ~Ac3Demuxer() override;

    Ac3Demuxer(const Ac3Demuxer&) = delete;
    Ac3Demuxer& operator=(const Ac3Demuxer&) = delete;

    bool open(const std::filesystem::path& path, std::string& error) override;
    const VideoTrackInfo& video() const override { return video_; }
    const AudioTrackInfo& audio() const override { return audio_; }
    double duration() const override { return duration_; }
    bool readSample(Sample& sample) override;
    bool seek(double seconds) override;

    std::vector<AudioTrackInfo> audioTracks() const override;
    bool selectAudioTrack(size_t index) override;
    bool readAudioSample(Sample& sample) override;

    // Frame size in bytes of the AC-3 syncframe at `data`, or 0 when it is not
    // a valid frame header. Shared with the program stream demuxer.
    static uint32_t frameSize(const uint8_t* data, size_t size);

private:
    struct FrameInfo {
        int64_t offset = 0;
        uint32_t size = 0;
        int64_t pts_ms = 0;
    };

    bool readAt(int64_t offset, void* data, size_t size);
    bool scanFrames(std::string& error);

    FILE* file_ = nullptr;
    std::filesystem::path path_;
    int64_t file_size_ = 0;
    VideoTrackInfo video_;
    AudioTrackInfo audio_;
    double duration_ = 0.0;
    std::vector<FrameInfo> frames_;
    size_t next_audio_ = 0;
};

}  // namespace player::demux
