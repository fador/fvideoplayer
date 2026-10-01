#pragma once

#include "demux/Demuxer.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <utility>
#include <vector>

namespace player::demux {

// Matroska/WebM demuxer for HEVC video tracks (V_MPEGH/ISO/HEVC).
// Blocks contain length-prefixed NAL units; the length size comes from the
// hvcC CodecPrivate. Text subtitle tracks (S_TEXT/UTF8, S_TEXT/ASS, ...) and
// all audio tracks are exposed for track selection.
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

    std::vector<AudioTrackInfo> audioTracks() const override { return audio_tracks_; }
    bool selectAudioTrack(size_t index) override;
    std::vector<SubtitleTrackInfo> subtitleTracks() const override { return subtitle_tracks_; }
    bool readSubtitleTrack(size_t index, std::vector<subtitle::SubtitleBlock>& blocks,
                           std::string& error) override;
    bool readAudioSample(Sample& sample) override;

    struct SampleInfo {
        uint64_t offset = 0;
        uint32_t size = 0;
        int64_t pts = 0;
        int64_t duration = 0;
        bool keyframe = false;
    };

private:
    struct TrackSamples {
        std::vector<SampleInfo> samples;
        size_t next = 0;
    };

    FILE* file_ = nullptr;
    std::filesystem::path path_;
    int64_t file_size_ = 0;
    VideoTrackInfo video_;
    AudioTrackInfo audio_;
    double duration_ = -1.0;
    std::vector<SampleInfo> samples_;
    std::vector<std::pair<int64_t, size_t>> keyframes_;  // pts -> sample index
    size_t next_sample_ = 0;

    std::vector<AudioTrackInfo> audio_tracks_;
    std::vector<TrackSamples> audio_samples_;
    size_t selected_audio_ = 0;

    std::vector<SubtitleTrackInfo> subtitle_tracks_;
    std::vector<TrackSamples> subtitle_samples_;
};

}  // namespace player::demux
