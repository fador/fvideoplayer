#pragma once

#include "demux/Demuxer.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

namespace player::demux {

// Raw MP3 (MPEG-1/2/2.5 Layer III) demuxer. Skips ID3v2 tags, indexes the
// frames (with an optional Xing/VBRI header for the duration) and exposes the
// stream as a single audio track. There is no video track.
class Mp3Demuxer final : public IDemuxer {
public:
    Mp3Demuxer();
    ~Mp3Demuxer() override;

    Mp3Demuxer(const Mp3Demuxer&) = delete;
    Mp3Demuxer& operator=(const Mp3Demuxer&) = delete;

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

    bool readAt(int64_t offset, void* data, size_t size);
    bool parseFrameHeader(const uint8_t* data, int* frame_size, int* samples, int* sample_rate);
    bool scanFrames(std::string& error);
    bool parseVbrHeader();

    FILE* file_ = nullptr;
    std::filesystem::path path_;
    int64_t file_size_ = 0;
    VideoTrackInfo video_;
    AudioTrackInfo audio_;
    double duration_ = 0.0;
    std::vector<FrameInfo> frames_;
    size_t next_audio_ = 0;
    int samples_per_frame_ = 1152;
    int sample_rate_ = 44100;
};

}  // namespace player::demux
