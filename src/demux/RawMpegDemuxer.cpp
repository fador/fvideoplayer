#include "demux/RawMpegDemuxer.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace player::demux {
namespace {

constexpr uint8_t kStartSequence = 0xB3;
constexpr uint8_t kStartExtension = 0xB5;
constexpr uint8_t kStartGop = 0xB8;
constexpr uint8_t kStartUserData = 0xB2;
constexpr uint8_t kStartPicture = 0x00;
constexpr uint8_t kStartSliceFirst = 0x01;
constexpr uint8_t kStartSliceLast = 0xAF;

bool isSliceStart(uint8_t code) {
    return code >= kStartSliceFirst && code <= kStartSliceLast;
}

bool seekFile(FILE* file, int64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, offset, SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

int64_t fileSize(FILE* file) {
#ifdef _WIN32
    return _ftelli64(file);
#else
    return static_cast<int64_t>(ftello(file));
#endif
}

struct StartCode {
    int64_t header = 0;  // exact offset of the `00 00 01` pattern
    int64_t body = 0;    // first payload byte after the code byte
    uint8_t code = 0;
};

bool findStartCode(const std::vector<uint8_t>& buffer, size_t from, StartCode& out) {
    for (size_t i = from; i + 3 < buffer.size(); ++i) {
        if (buffer[i] != 0 || buffer[i + 1] != 0 || buffer[i + 2] != 1) {
            continue;
        }
        out.header = static_cast<int64_t>(i);
        out.body = static_cast<int64_t>(i) + 4;
        out.code = buffer[i + 3];
        return true;
    }
    return false;
}

// Returns the fields of a picture header that the demuxer needs.
struct PictureInfo {
    uint8_t type = 1;
    bool valid = false;
};

PictureInfo parsePictureHeader(const std::vector<uint8_t>& buffer, int64_t offset, int64_t limit) {
    PictureInfo info;
    if (offset + 4 > limit) {
        return info;
    }
    // temporal_reference is 10 bits, then the 3-bit picture_coding_type.
    const uint32_t bits = (static_cast<uint32_t>(buffer[static_cast<size_t>(offset)]) << 24) |
                          (static_cast<uint32_t>(buffer[static_cast<size_t>(offset) + 1]) << 16) |
                          (static_cast<uint32_t>(buffer[static_cast<size_t>(offset) + 2]) << 8) |
                          static_cast<uint32_t>(buffer[static_cast<size_t>(offset) + 3]);
    info.type = static_cast<uint8_t>((bits >> 19) & 0x07);
    info.valid = info.type >= 1 && info.type <= 3;
    return info;
}

// Splits the stream into pictures and reads the geometry from the sequence
// headers. Also reports whether the stream is MPEG-2, which is decided by a
// sequence extension (an extension start code with id 1) following the
// sequence header.
void scanPictures(const std::vector<uint8_t>& buffer, int64_t file_size,
                  std::vector<RawMpegDemuxer::PictureRange>* pictures, uint32_t* width,
                  uint32_t* height, bool* mpeg2) {
    StartCode sc;
    if (!findStartCode(buffer, 0, sc)) {
        return;
    }

    int64_t picture_start = -1;
    // Start of the sequence or group header block that has not been emitted yet.
    int64_t header_start = -1;
    PictureInfo picture_info{};
    bool awaiting_extension = false;

    for (;;) {
        if (sc.code == kStartSequence) {
            // The first four bytes carry horizontal_size, vertical_size,
            // pixel_aspect_ratio and frame_rate_code.
            if (sc.body + 4 <= static_cast<int64_t>(buffer.size())) {
                const size_t at = static_cast<size_t>(sc.body);
                const uint32_t bits =
                    (static_cast<uint32_t>(buffer[at]) << 24) |
                    (static_cast<uint32_t>(buffer[at + 1]) << 16) |
                    (static_cast<uint32_t>(buffer[at + 2]) << 8) |
                    static_cast<uint32_t>(buffer[at + 3]);
                *width = (bits >> 20) & 0x0FFF;
                *height = (bits >> 8) & 0x0FFF;
            }
            if (picture_start < 0) {
                header_start = sc.header;
            }
            awaiting_extension = true;
        } else if (sc.code == kStartExtension && awaiting_extension) {
            // The extension id is the top nibble of the first payload byte.
            if (sc.body < static_cast<int64_t>(buffer.size()) &&
                (buffer[static_cast<size_t>(sc.body)] >> 4) == 1) {
                *mpeg2 = true;
            }
            awaiting_extension = false;
        } else if (sc.code == kStartPicture) {
            awaiting_extension = false;
            if (picture_start >= 0) {
                pictures->push_back(
                    {picture_start, sc.header - picture_start, picture_info.type});
            }
            // Carry any not-yet-emitted sequence or group header along.
            picture_start = header_start >= 0 ? header_start : sc.header;
            picture_info = parsePictureHeader(buffer, sc.body, file_size);
            header_start = -1;
        } else if (sc.code == 0xB7) {
            if (picture_start >= 0) {
                pictures->push_back(
                    {picture_start, sc.header - picture_start, picture_info.type});
            }
            picture_start = -1;
            break;
        } else if (picture_start < 0 && (sc.code == kStartGop || sc.code == kStartUserData)) {
            header_start = header_start >= 0 ? header_start : sc.header;
        }

        if (!findStartCode(buffer, static_cast<size_t>(sc.body), sc)) {
            break;
        }
    }

    if (picture_start >= 0 && file_size > picture_start) {
        pictures->push_back({picture_start, file_size - picture_start, picture_info.type});
    }
}

}  // namespace

RawMpegDemuxer::RawMpegDemuxer(double fps) : fps_(fps > 0.0 ? fps : 25.0) {}

RawMpegDemuxer::~RawMpegDemuxer() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool RawMpegDemuxer::open(const std::filesystem::path& path, std::string& error) {
    file_ = std::fopen(path.string().c_str(), "rb");
    if (!file_) {
        error = "failed to open " + path.string();
        return false;
    }
    if (std::fseek(file_, 0, SEEK_END) != 0) {
        error = "failed to determine file size";
        return false;
    }
    const int64_t size = fileSize(file_);
    if (size <= 0) {
        error = "empty file";
        return false;
    }
    if (!seekFile(file_, 0)) {
        error = "failed to seek input";
        return false;
    }
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    if (std::fread(buffer.data(), 1, buffer.size(), file_) != buffer.size()) {
        error = "failed to read input";
        return false;
    }

    uint32_t width = 0;
    uint32_t height = 0;
    bool mpeg2 = false;
    scanPictures(buffer, size, &pictures_, &width, &height, &mpeg2);
    if (pictures_.empty()) {
        error = "no MPEG-1 pictures found in stream";
        return false;
    }

    video_ = VideoTrackInfo{};
    video_.codec = mpeg2 ? Codec::Mpeg2 : Codec::Mpeg1;
    video_.width = width;
    video_.height = height;
    video_.fps = fps_;
    video_.timescale = 1000;
    // The payload is a start-code prefixed elementary stream.
    video_.format.annexb = true;

    next_picture_ = 0;
    return true;
}

double RawMpegDemuxer::duration() const {
    if (pictures_.empty()) {
        return -1.0;
    }
    return static_cast<double>(pictures_.size()) / fps_;
}

bool RawMpegDemuxer::readSample(Sample& sample) {
    if (!file_ || next_picture_ >= pictures_.size()) {
        return false;
    }
    const PictureRange& picture = pictures_[next_picture_];
    sample.data.resize(static_cast<size_t>(picture.size));
    if (!seekFile(file_, picture.offset)) {
        return false;
    }
    if (std::fread(sample.data.data(), 1, sample.data.size(), file_) != sample.data.size()) {
        return false;
    }

    const int64_t pts =
        static_cast<int64_t>(std::llround(static_cast<double>(next_picture_) * 1000.0 / fps_));
    sample.pts = pts;
    sample.dts = pts;
    sample.keyframe = picture.type == 1;
    sample.track = TrackType::Video;
    ++next_picture_;
    return true;
}

bool RawMpegDemuxer::seek(double seconds) {
    if (pictures_.empty()) {
        return false;
    }
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    size_t target = static_cast<size_t>(seconds * fps_);
    if (target >= pictures_.size()) {
        target = pictures_.size() - 1;
    }

    // Back up to the nearest I picture, which the decoder can start from.
    size_t index = 0;
    for (size_t i = target + 1; i-- > 0;) {
        if (pictures_[i].type == 1) {
            index = i;
            break;
        }
    }
    next_picture_ = index;
    return true;
}

}  // namespace player::demux

