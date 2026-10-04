#pragma once

#include "demux/Demuxer.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace player::demux {

// Flash Video (FLV) demuxer. The tag list is walked once at open time, which
// gives the duration, the frame rate and a keyframe index for seeking without
// relying on the onMetaData script tag, although that is read as well when it
// is present.
//
// Video is Sorenson Spark (FLV1); audio is MP3 or AAC. Other FLV video codecs
// are named but not indexed, so the audio still plays; without audio such a
// file is rejected with a message naming the codec.
class FlvDemuxer final : public IDemuxer {
public:
    FlvDemuxer();
    ~FlvDemuxer() override;

    FlvDemuxer(const FlvDemuxer&) = delete;
    FlvDemuxer& operator=(const FlvDemuxer&) = delete;

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
    struct TagInfo {
        int64_t offset = 0;  // offset of the tag header, not of its payload
        uint32_t size = 0;   // payload size in bytes
        int64_t pts_ms = 0;
        bool keyframe = false;
    };

    bool readAt(int64_t offset, void* data, size_t size);
    bool readPayload(const TagInfo& tag, std::vector<uint8_t>& data);
    bool scanTags(std::string& error);
    void configureAudio(uint8_t sound_format, const uint8_t* payload, size_t size);
    void finishTracks();

    FILE* file_ = nullptr;
    std::filesystem::path path_;
    int64_t file_size_ = 0;
    VideoTrackInfo video_;
    AudioTrackInfo audio_;
    double duration_ = 0.0;
    std::vector<TagInfo> video_tags_;
    std::vector<TagInfo> audio_tags_;
    size_t next_video_ = 0;
    size_t next_audio_ = 0;
    bool have_duration_ = false;
    bool have_fps_ = false;
};

}  // namespace player::demux
