#include "demux/Mp3Demuxer.hpp"

#include <algorithm>
#include <cstring>

namespace player::demux {
namespace {

// MPEG audio bitrate tables (kbps), indexed [mpeg1][layer-1][bitrate index].
constexpr int kBitrateTable[2][3][15] = {
    {{0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448},
     {0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384},
     {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320}},
    {{0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256},
     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160},
     {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160}},
};

constexpr int kSampleRateTable[3] = {44100, 48000, 32000};

struct FrameHeader {
    bool valid = false;
    int version = 1;  // 1, 2 or 25 (MPEG-2.5)
    int layer = 3;
    int bitrate_kbps = 0;
    int sample_rate = 0;
    int padding = 0;
    int channels = 2;
    int samples = 1152;
    int frame_size = 0;
    bool crc = false;
};

FrameHeader parseHeader(const uint8_t* h) {
    FrameHeader f;
    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
        return f;
    }
    const int version_bits = (h[1] >> 3) & 3;
    const int layer_bits = (h[1] >> 1) & 3;
    const int bitrate_index = h[2] >> 4;
    const int rate_index = (h[2] >> 2) & 3;
    if (version_bits == 1 || layer_bits == 0 || bitrate_index == 0 || bitrate_index == 15 ||
        rate_index == 3) {
        return f;
    }

    f.version = version_bits == 3 ? 1 : (version_bits == 2 ? 2 : 25);
    f.layer = 4 - layer_bits;
    f.crc = (h[1] & 1) == 0;
    const int mpeg1 = f.version == 1 ? 0 : 1;
    f.bitrate_kbps = kBitrateTable[mpeg1][f.layer - 1][bitrate_index];
    f.sample_rate = kSampleRateTable[rate_index] / (f.version == 1 ? 1 : (f.version == 2 ? 2 : 4));
    f.padding = (h[2] >> 1) & 1;
    f.channels = ((h[3] >> 6) & 3) == 3 ? 1 : 2;
    f.samples = f.layer == 1 ? 384 : (f.layer == 3 && f.version != 1 ? 576 : 1152);
    f.frame_size = f.samples / 8 * f.bitrate_kbps * 1000 / f.sample_rate + f.padding;
    f.valid = f.frame_size > 4;
    return f;
}

bool sameStream(const FrameHeader& a, const FrameHeader& b) {
    return a.version == b.version && a.layer == b.layer && a.sample_rate == b.sample_rate;
}

}  // namespace

Mp3Demuxer::Mp3Demuxer() = default;

Mp3Demuxer::~Mp3Demuxer() {
    if (file_) {
        std::fclose(file_);
    }
}

bool Mp3Demuxer::readAt(int64_t offset, void* data, size_t size) {
    if (!file_ || offset < 0 || offset + static_cast<int64_t>(size) > file_size_) {
        return false;
    }
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }
    return std::fread(data, 1, size, file_) == size;
}

bool Mp3Demuxer::parseFrameHeader(const uint8_t* data, int* frame_size, int* samples,
                                  int* sample_rate) {
    const FrameHeader header = parseHeader(data);
    if (!header.valid) {
        return false;
    }
    *frame_size = header.frame_size;
    *samples = header.samples;
    *sample_rate = header.sample_rate;
    return true;
}

bool Mp3Demuxer::scanFrames(std::string& error) {
    // Skip an ID3v2 tag when present.
    int64_t offset = 0;
    uint8_t tag[10];
    if (readAt(0, tag, sizeof(tag)) && tag[0] == 'I' && tag[1] == 'D' && tag[2] == '3') {
        const int64_t size = (static_cast<int64_t>(tag[6] & 0x7F) << 21) |
                             (static_cast<int64_t>(tag[7] & 0x7F) << 14) |
                             (static_cast<int64_t>(tag[8] & 0x7F) << 7) |
                             static_cast<int64_t>(tag[9] & 0x7F);
        offset = 10 + size + ((tag[5] & 0x10) != 0 ? 10 : 0);
    }

    // Find the first frame: two consecutive matching headers.
    uint8_t buffer[4];
    bool found = false;
    for (; offset + 4 <= file_size_ && offset < file_size_; ++offset) {
        if (!readAt(offset, buffer, 4)) {
            break;
        }
        const FrameHeader first = parseHeader(buffer);
        if (!first.valid) {
            continue;
        }
        uint8_t next[4];
        if (!readAt(offset + first.frame_size, next, 4)) {
            continue;
        }
        const FrameHeader second = parseHeader(next);
        if (second.valid && sameStream(first, second)) {
            found = true;
            break;
        }
    }
    if (!found) {
        error = "no MP3 frame found";
        return false;
    }

    sample_rate_ = 0;
    samples_per_frame_ = 0;
    int64_t total_samples = 0;
    uint8_t header_bytes[4];
    while (offset + 4 <= file_size_) {
        if (!readAt(offset, header_bytes, 4)) {
            break;
        }
        const FrameHeader header = parseHeader(header_bytes);
        if (!header.valid || offset + header.frame_size > file_size_) {
            break;
        }
        if (sample_rate_ == 0) {
            sample_rate_ = header.sample_rate;
            samples_per_frame_ = header.samples;
        } else if (header.sample_rate != sample_rate_) {
            break;
        }

        FrameInfo info;
        info.offset = offset;
        info.size = static_cast<uint32_t>(header.frame_size);
        info.pts_ms = total_samples * 1000 / sample_rate_;
        frames_.push_back(info);
        total_samples += header.samples;
        offset += header.frame_size;
    }
    if (frames_.empty()) {
        error = "no MP3 frames";
        return false;
    }

    audio_.index = 0;
    audio_.name = "MP3";
    audio_.language = "und";
    audio_.codec = "mp3";
    audio_.present = true;
    audio_.default_track = true;
    audio_.codec_fourcc = 0x6D703320;  // 'mp3 '
    uint8_t first_header[4];
    if (readAt(frames_[0].offset, first_header, 4)) {
        audio_.channels = static_cast<uint32_t>(parseHeader(first_header).channels);
    }
    audio_.sample_rate = static_cast<uint32_t>(sample_rate_);
    video_.timescale = 1000;
    duration_ = static_cast<double>(total_samples) / sample_rate_;

    // A Xing/Info or VBRI header carries the exact frame count (VBR).
    parseVbrHeader();
    return true;
}

bool Mp3Demuxer::parseVbrHeader() {
    if (frames_.empty()) {
        return false;
    }
    const FrameInfo& first = frames_[0];
    uint8_t header_bytes[4];
    if (!readAt(first.offset, header_bytes, 4)) {
        return false;
    }
    const FrameHeader header = parseHeader(header_bytes);
    if (!header.valid) {
        return false;
    }

    const int side_info = header.version == 1 ? (header.channels == 1 ? 17 : 32)
                                              : (header.channels == 1 ? 9 : 17);
    const int64_t tag_offset = first.offset + 4 + (header.crc ? 2 : 0) + side_info;
    uint8_t tag[16];
    int frame_count = 0;
    if (readAt(tag_offset, tag, sizeof(tag)) &&
        (std::memcmp(tag, "Xing", 4) == 0 || std::memcmp(tag, "Info", 4) == 0)) {
        const uint32_t flags = (static_cast<uint32_t>(tag[4]) << 24) |
                               (tag[5] << 16) | (tag[6] << 8) | tag[7];
        if ((flags & 1) != 0) {
            frame_count = static_cast<int>((static_cast<uint32_t>(tag[8]) << 24) |
                                           (tag[9] << 16) | (tag[10] << 8) | tag[11]);
        }
    } else if (readAt(first.offset + 36, tag, sizeof(tag)) && std::memcmp(tag, "VBRI", 4) == 0) {
        frame_count = static_cast<int>((static_cast<uint32_t>(tag[14]) << 24) |
                                       (tag[15] << 16) | (tag[16] << 8) | tag[17]);
    }

    if (frame_count > 0) {
        duration_ = static_cast<double>(frame_count) * samples_per_frame_ / sample_rate_;
    }
    return true;
}

bool Mp3Demuxer::open(const std::filesystem::path& path, std::string& error) {
    path_ = path;
    file_ = std::fopen(path.string().c_str(), "rb");
    if (!file_) {
        error = "cannot open " + path.string();
        return false;
    }
    std::fseek(file_, 0, SEEK_END);
    file_size_ = std::ftell(file_);
    std::fseek(file_, 0, SEEK_SET);

    frames_.clear();
    next_audio_ = 0;
    if (!scanFrames(error)) {
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    return true;
}

std::vector<AudioTrackInfo> Mp3Demuxer::audioTracks() const {
    if (!audio_.present) {
        return {};
    }
    return {audio_};
}

bool Mp3Demuxer::selectAudioTrack(size_t index) {
    return index == 0 && audio_.present;
}

bool Mp3Demuxer::readSample(Sample& sample) {
    (void)sample;
    return false;  // no video track
}

bool Mp3Demuxer::readAudioSample(Sample& sample) {
    if (next_audio_ >= frames_.size()) {
        return false;
    }
    const FrameInfo& info = frames_[next_audio_++];
    sample.data.resize(info.size);
    if (!readAt(info.offset, sample.data.data(), info.size)) {
        return false;
    }
    sample.pts = info.pts_ms;
    sample.dts = info.pts_ms;
    sample.keyframe = true;
    sample.track = TrackType::Audio;
    return true;
}

bool Mp3Demuxer::seek(double seconds) {
    if (frames_.empty()) {
        return false;
    }
    const int64_t target_ms = static_cast<int64_t>(seconds * 1000.0);
    const auto it = std::upper_bound(frames_.begin(), frames_.end(), target_ms,
                                     [](int64_t value, const FrameInfo& info) {
                                         return value < info.pts_ms;
                                     });
    next_audio_ = static_cast<size_t>(it - frames_.begin());
    if (next_audio_ > 0) {
        --next_audio_;  // start at the frame containing the target time
    }
    return true;
}

}  // namespace player::demux
