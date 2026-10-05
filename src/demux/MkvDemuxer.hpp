#pragma once

#include "demux/Demuxer.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <unordered_map>
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

    // Track number -> (kind, index): 0 video, 1 audio, 2 subtitle.
    struct TrackRef {
        int kind = -1;
        size_t index = 0;
    };

    // Cue index entry from the Matroska Cues element: a keyframe time and the
    // cluster that holds it (offset relative to the Segment payload).
    struct Cue {
        int64_t time_ms = 0;
        int64_t cluster_offset = 0;
    };

    // Parses top-level elements from index_pos_ until one cluster is consumed,
    // appending its samples. Returns false at the end of the segment.
    bool parseNextCluster();
    // Parses the children of one cluster, appending blocks to the given sample
    // lists (null pointers skip that kind). Returns the offset of the next
    // top-level element (the cluster end, or where the next element starts for
    // an unknown-size cluster).
    int64_t parseCluster(FILE* file, int64_t payload, int64_t cluster_end, bool unknown,
                         std::vector<SampleInfo>* video, std::vector<TrackSamples>* audio,
                         std::vector<TrackSamples>* subtitle);
    void appendBlockSamples(FILE* file, int64_t block_payload, uint64_t block_size,
                            int64_t cluster_timestamp, int keyframe_override,
                            int64_t block_duration, std::vector<SampleInfo>* video,
                            std::vector<TrackSamples>* audio,
                            std::vector<TrackSamples>* subtitle);
    int64_t toMs(int64_t ticks) const {
        return timestamp_scale_ == 1000000
                   ? ticks
                   : ticks * static_cast<int64_t>(timestamp_scale_) / 1000000;
    }

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

    // Lazy sample indexing. The cluster scan runs while reading, and seeking
    // jumps to the keyframe from the Cues index instead of building the whole
    // sample table when the file is opened.
    std::unordered_map<uint64_t, TrackRef> track_map_;
    std::vector<Cue> cues_;
    int64_t segment_start_ = 0;
    int64_t segment_end_ = 0;
    uint64_t timestamp_scale_ = 1000000;
    int64_t index_pos_ = -1;
    bool index_complete_ = true;
    bool full_index_ = true;
};

}  // namespace player::demux
