#include "demux/FlacDemuxer.hpp"

#include "audio/flac/FlacDecoder.hpp"

#include <algorithm>
#include <cstring>

namespace player::demux {
namespace {

uint32_t readU32Be(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

uint64_t readU64Be(const uint8_t* data) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

constexpr uint8_t kBlockStreamInfo = 0;
constexpr uint8_t kBlockSeekTable = 3;

}  // namespace

FlacDemuxer::FlacDemuxer() = default;

FlacDemuxer::~FlacDemuxer() {
    if (file_) {
        std::fclose(file_);
    }
}

bool FlacDemuxer::readAt(int64_t offset, void* data, size_t size) {
    if (!file_ || offset < 0 || offset + static_cast<int64_t>(size) > file_size_) {
        return false;
    }
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }
    return std::fread(data, 1, size, file_) == size;
}

bool FlacDemuxer::parseMetadata(std::string& error) {
    uint8_t marker[4];
    if (!readAt(0, marker, 4) || std::memcmp(marker, "fLaC", 4) != 0) {
        error = "not a FLAC stream";
        return false;
    }
    if (!readAt(4, marker, 4) || (marker[0] & 0x7F) != kBlockStreamInfo) {
        error = "FLAC STREAMINFO must be the first metadata block";
        return false;
    }
    const uint32_t info_size = (static_cast<uint32_t>(marker[1]) << 16) |
                               (static_cast<uint32_t>(marker[2]) << 8) | marker[3];
    if (info_size < 34) {
        error = "short FLAC STREAMINFO block";
        return false;
    }
    std::vector<uint8_t> info(info_size);
    if (!readAt(8, info.data(), info_size)) {
        error = "truncated FLAC STREAMINFO block";
        return false;
    }

    block_size_ = static_cast<uint32_t>((info[2] << 8) | info[3]);
    const uint8_t* packed = info.data() + 10;
    sample_rate_ = static_cast<uint32_t>((static_cast<uint64_t>(packed[0]) << 12) |
                                         (static_cast<uint64_t>(packed[1]) << 4) |
                                         (static_cast<uint64_t>(packed[2]) >> 4));
    channels_ = static_cast<uint32_t>(((packed[2] >> 1) & 7) + 1);
    bits_ = static_cast<uint32_t>((((packed[2] & 1) << 4) | (packed[3] >> 4)) + 1);
    total_samples_ = ((static_cast<uint64_t>(packed[3] & 0x0F) << 32) |
                      (static_cast<uint64_t>(packed[4]) << 24) |
                      (static_cast<uint64_t>(packed[5]) << 16) |
                      (static_cast<uint64_t>(packed[6]) << 8) | packed[7]);
    if (sample_rate_ == 0 || channels_ == 0 || channels_ > 8 || bits_ < 4 || bits_ > 32) {
        error = "invalid FLAC STREAMINFO";
        return false;
    }

    // Walk the remaining metadata blocks; only the seek table is used.
    int64_t offset = 8 + info_size;
    bool last = false;
    while (!last) {
        uint8_t head[4];
        if (!readAt(offset, head, 4)) {
            error = "truncated FLAC metadata";
            return false;
        }
        last = (head[0] & 0x80) != 0;
        const uint8_t type = head[0] & 0x7F;
        const uint32_t size = (static_cast<uint32_t>(head[1]) << 16) |
                              (static_cast<uint32_t>(head[2]) << 8) | head[3];
        if (type == kBlockSeekTable && size >= 18) {
            std::vector<uint8_t> table(size);
            if (readAt(offset + 4, table.data(), size)) {
                const size_t points = size / 18;
                seektable_.reserve(points);
                for (size_t i = 0; i < points; ++i) {
                    const uint8_t* point = table.data() + i * 18;
                    SeekPoint entry;
                    entry.first_sample = readU64Be(point);
                    entry.byte_offset = readU64Be(point + 8);
                    entry.frame_samples = readU32Be(point + 16);
                    seektable_.push_back(entry);
                }
            }
        }
        offset += 4 + size;
        if (offset > file_size_) {
            error = "truncated FLAC metadata";
            return false;
        }
    }
    first_frame_offset_ = offset;

    // The decoder takes "fLaC" plus the STREAMINFO block as its configuration,
    // so hand exactly that over as track extradata.
    stream_info_.resize(8 + info.size());
    if (!readAt(0, stream_info_.data(), stream_info_.size())) {
        error = "truncated FLAC STREAMINFO block";
        return false;
    }

    audio_.index = 0;
    audio_.name = "FLAC";
    audio_.language = "und";
    audio_.codec = "flac";
    audio_.present = true;
    audio_.default_track = true;
audio_.codec_fourcc = 0x664C6143;  // 'fLaC'
    audio_.channels = channels_;
    audio_.sample_rate = sample_rate_;
    audio_.extradata = stream_info_;
    video_.timescale = 1000;
    duration_ = static_cast<double>(total_samples_) / sample_rate_;
    return true;
}

bool FlacDemuxer::indexFrames(std::string& error) {
    audio::flac::Decoder decoder;
    // The stream parameters come from STREAMINFO so the frame header codes that
    // mean "same as the stream" resolve correctly.
    if (!decoder.configure(stream_info_.data(), stream_info_.size(), error)) {
        return false;
    }

    int64_t offset = first_frame_offset_;
    uint64_t total_samples = 0;
    std::vector<uint8_t> buffer(65536);
    while (offset + 5 <= file_size_) {
        const int64_t remaining = file_size_ - offset;
        const size_t chunk = static_cast<size_t>(std::min<int64_t>(remaining, buffer.size()));
        if (!readAt(offset, buffer.data(), chunk)) {
            break;
        }
        uint32_t block_size = 0;
        const size_t frame_bytes =
            decoder.skipFrame(buffer.data(), chunk, &block_size);
        if (frame_bytes == 0) {
            // Resync: look for the next frame header.
            bool found = false;
            for (size_t i = 2; i + 5 <= chunk; ++i) {
                if (buffer[i] == 0xFF && (buffer[i + 1] & 0xFC) == 0xF8) {
                    const size_t found_bytes =
                        decoder.skipFrame(buffer.data() + i, chunk - i, &block_size);
                    if (found_bytes != 0) {
                        offset += static_cast<int64_t>(i);
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                break;
            }
            continue;
        }

        FrameInfo info;
        info.offset = offset;
        info.size = static_cast<uint32_t>(frame_bytes);
        info.pts_ms = static_cast<int64_t>(total_samples) * 1000 / sample_rate_;
        frames_.push_back(info);
        total_samples += block_size;
        offset += static_cast<int64_t>(frame_bytes);
    }

    if (frames_.empty()) {
        error = "no FLAC frames found";
        return false;
    }
    if (total_samples_ == 0) {
        total_samples_ = total_samples;
        duration_ = static_cast<double>(total_samples_) / sample_rate_;
    }
    return true;
}

bool FlacDemuxer::open(const std::filesystem::path& path, std::string& error) {
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
    seektable_.clear();
    next_audio_ = 0;
    if (!parseMetadata(error) || !indexFrames(error)) {
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    return true;
}

std::vector<AudioTrackInfo> FlacDemuxer::audioTracks() const {
    if (!audio_.present) {
        return {};
    }
    return {audio_};
}

bool FlacDemuxer::selectAudioTrack(size_t index) {
    return index == 0 && audio_.present;
}

bool FlacDemuxer::readSample(Sample& sample) {
    (void)sample;
    return false;  // no video track
}

bool FlacDemuxer::readAudioSample(Sample& sample) {
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

bool FlacDemuxer::seek(double seconds) {
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
