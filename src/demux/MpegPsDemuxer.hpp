#pragma once

#include "demux/Demuxer.hpp"
#include "demux/MpegEsScan.hpp"

namespace player::demux {

// MPEG-1 and MPEG-2 program stream demuxer (.mpg, .vob, .m2p).
//
// A program stream interleaves pack headers, a system header and PES packets.
// The video PES packets are reassembled and split into pictures exactly like a
// raw stream; the first audio stream is reassembled and split into MPEG audio
// frames so the player can decode Layer II or Layer III.
class MpegPsDemuxer : public IDemuxer {
public:
    bool open(const std::filesystem::path& path, std::string& error) override;
    const VideoTrackInfo& video() const override { return video_; }
    const AudioTrackInfo& audio() const override { return audio_; }
    double duration() const override;
    bool readSample(Sample& sample) override;
    bool seek(double seconds) override;

    std::vector<AudioTrackInfo> audioTracks() const override;
    bool selectAudioTrack(size_t index) override;
    bool readAudioSample(Sample& sample) override;

private:
    // Where a PES packet's payload landed in the reassembled elementary
    // stream, with the time it carried (-1 when it carried none).
    struct PtsMark {
        size_t es_offset = 0;
        int64_t pts = -1;
    };

    struct AudioFrame {
        int64_t offset = 0;
        int64_t size = 0;
        int64_t pts_ms = 0;
    };

    // Splits the collected audio elementary stream into MPEG audio frames and
    // fills in the track description.
    void scanAudioTrack(const std::vector<PtsMark>& marks);

    std::vector<uint8_t> video_es_;
    std::vector<MpegEsPicture> pictures_;
    std::vector<uint8_t> audio_es_;
    std::vector<AudioFrame> audio_frames_;
    VideoTrackInfo video_{};
    AudioTrackInfo audio_{};
    double fps_ = 25.0;
    size_t next_picture_ = 0;
    size_t next_audio_ = 0;
};

}  // namespace player::demux
