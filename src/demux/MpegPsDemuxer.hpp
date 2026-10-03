#pragma once

#include "demux/Demuxer.hpp"
#include "demux/MpegEsScan.hpp"

namespace player::demux {

// MPEG-1 and MPEG-2 program stream demuxer (.mpg, .vob, .m2p).
//
// A program stream interleaves pack headers, a system header and PES packets.
// Only the video PES packets are decoded here; the elementary stream they
// carry is reassembled and split into pictures exactly like a raw stream.
class MpegPsDemuxer : public IDemuxer {
public:
    bool open(const std::filesystem::path& path, std::string& error) override;
    const VideoTrackInfo& video() const override { return video_; }
    const AudioTrackInfo& audio() const override { return audio_; }
    double duration() const override;
    bool readSample(Sample& sample) override;
    bool seek(double seconds) override;

private:
    std::vector<uint8_t> video_es_;
    std::vector<MpegEsPicture> pictures_;
    VideoTrackInfo video_{};
    AudioTrackInfo audio_{};
    double fps_ = 25.0;
    size_t next_picture_ = 0;
};

}  // namespace player::demux
