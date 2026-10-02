#pragma once

#include "demux/Demuxer.hpp"

#include <cstdint>
#include <cstdio>
#include <utility>
#include <vector>

namespace player::demux {

// ISO Base Media File Format (MP4/MOV) demuxer for HEVC video tracks.
// Supports non-fragmented files with hvc1/hev1 sample entries, the audio
// tracks (AAC in mp4a entries, Opus in its own entry) and tx3g timed text.
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
    bool readAudioSample(Sample& sample) override;
    std::vector<AudioTrackInfo> audioTracks() const override { return audio_tracks_; }
    bool selectAudioTrack(size_t index) override;
    std::vector<SubtitleTrackInfo> subtitleTracks() const override { return subtitle_tracks_; }
    bool readSubtitleTrack(size_t index, std::vector<subtitle::SubtitleBlock>& blocks,
                           std::string& error) override;

    struct SampleInfo {
        uint64_t offset = 0;
        uint32_t size = 0;
        int64_t dts = 0;
        int64_t pts = 0;
        int64_t duration = 0;
        bool keyframe = false;
    };

    // Per-track packet list. Only the video track is read sequentially; the
    // audio cursors are moved by seek() so playback follows the playhead.
    struct TrackSamples {
        std::vector<SampleInfo> samples;
        size_t next = 0;
        uint32_t timescale = 1000;
    };

private:
    std::filesystem::path path_;
    FILE* file_ = nullptr;
    VideoTrackInfo video_;
    AudioTrackInfo audio_;
    double duration_ = -1.0;
    std::vector<SampleInfo> samples_;
    std::vector<std::pair<int64_t, size_t>> keyframes_;  // pts -> sample index
    size_t next_sample_ = 0;
    std::vector<AudioTrackInfo> audio_tracks_;
    std::vector<TrackSamples> audio_samples_;
    std::vector<SubtitleTrackInfo> subtitle_tracks_;
    std::vector<TrackSamples> subtitle_samples_;
    size_t selected_audio_ = 0;
};

}  // namespace player::demux
