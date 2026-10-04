#include "demux/MpegPsDemuxer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace player::demux {
namespace {

// Program stream start codes (ISO/IEC 11172-1 and 13818-1).
constexpr uint8_t kPackStart = 0xBA;
constexpr uint8_t kSystemHeader = 0xBB;
constexpr uint8_t kProgramStreamMap = 0xBC;
constexpr uint8_t kPrivateStream1 = 0xBD;
constexpr uint8_t kPaddingStream = 0xBE;
constexpr uint8_t kPrivateStream2 = 0xBF;
constexpr uint8_t kProgramEnd = 0xB9;

// Elementary stream start codes must never appear at the top level of a
// program stream; seeing one means this is not a program stream after all.
bool isElementaryStartCode(uint8_t code) {
    return code == 0x00 || (code >= 0x01 && code <= 0xAF) || code == 0xB2 || code == 0xB3 ||
           code == 0xB5 || code == 0xB7 || code == 0xB8;
}

bool isVideoStreamId(uint8_t id) {
    return id >= 0xE0 && id <= 0xEF;
}

bool isAudioStreamId(uint8_t id) {
    return (id >= 0xC0 && id <= 0xDF) || id == kPrivateStream1;
}

uint32_t readBe16(const std::vector<uint8_t>& buffer, size_t at) {
    return (static_cast<uint32_t>(buffer[at]) << 8) | buffer[at + 1];
}

// A 5-byte PTS or DTS field: three groups of bits separated by marker bits.
// The markers are ignored, exactly as the reference implementation does.
int64_t parsePtsField(const std::vector<uint8_t>& buffer, size_t at) {
    const int64_t high = (buffer[at] & 0x0E);
    const int64_t middle = readBe16(buffer, at + 1) >> 1;
    const int64_t low = readBe16(buffer, at + 3) >> 1;
    return (high << 29) | (middle << 15) | low;
}

// Finds the next start code whose code byte is a system layer code, used to
// bound a PES packet whose declared length is zero.
bool findSystemStartCode(const std::vector<uint8_t>& buffer, size_t from, size_t& position) {
    for (size_t i = from; i + 3 < buffer.size(); ++i) {
        if (buffer[i] != 0 || buffer[i + 1] != 0 || buffer[i + 2] != 1) {
            continue;
        }
        if (isElementaryStartCode(buffer[i + 3])) {
            continue;
        }
        position = i;
        return true;
    }
    return false;
}

// Parses the optional PES header starting just after the 16-bit packet length.
// On success `p` points at the payload, `payload` holds its length (or
// kUnbounded when the stream declares none) and `pts` is -1 when absent.
constexpr int64_t kUnbounded = -1;

bool parsePesHeader(const std::vector<uint8_t>& buffer, size_t& p, int64_t packet_length,
                    int64_t& pts, int64_t& payload) {
    pts = -1;
    int64_t remaining = packet_length;
    if (p + 1 > buffer.size()) {
        return false;
    }

    // Stuffing bytes before the optional header.
    uint8_t c = 0;
    for (;;) {
        if (remaining < 1 || p >= buffer.size()) {
            return false;
        }
        c = buffer[p++];
        --remaining;
        if (c != 0xFF) {
            break;
        }
    }

    // MPEG-1 puts a two byte STD buffer field here; MPEG-2 does not.
    if ((c & 0xC0) == 0x40) {
        if (remaining < 2 || p + 1 > buffer.size()) {
            return false;
        }
        c = buffer[p + 1];
        p += 2;
        remaining -= 2;
    }

    if ((c & 0xE0) == 0x20) {
        // MPEG-1: the byte just read starts the five byte PTS field.
        if (remaining < 4 || p + 3 > buffer.size()) {
            return false;
        }
        pts = parsePtsField(buffer, p - 1);
        p += 4;
        remaining -= 4;
        if ((c & 0x10) != 0) {
            if (remaining < 5 || p + 4 > buffer.size()) {
                return false;
            }
            p += 5;
            remaining -= 5;
        }
    } else if ((c & 0xC0) == 0x80) {
        // MPEG-2: one flags byte and one header length byte.
        if (remaining < 2 || p + 1 > buffer.size()) {
            return false;
        }
        const uint8_t flags = buffer[p];
        const uint8_t header_length = buffer[p + 1];
        p += 2;
        remaining -= 2;
        if (header_length > remaining) {
            return false;
        }
        const size_t header_start = p;
        if ((flags & 0x80) != 0 && p + 4 < buffer.size()) {
            pts = parsePtsField(buffer, p);
        }
        p = header_start + header_length;
        remaining -= header_length;
    } else {
        // A bare header with no optional fields.
        --p;
        ++remaining;
    }

    if (packet_length == 0) {
        payload = kUnbounded;
    } else {
        payload = remaining;
    }
    return true;
}

bool readWholeFile(const std::filesystem::path& path, std::vector<uint8_t>& out,
                   std::string& error) {
    FILE* file = std::fopen(path.string().c_str(), "rb");
    if (file == nullptr) {
        error = "failed to open " + path.string();
        return false;
    }
    if (std::fseek(file, 0, SEEK_END) != 0) {
        std::fclose(file);
        error = "failed to determine file size";
        return false;
    }
#ifdef _WIN32
    const int64_t size = _ftelli64(file);
#else
    const int64_t size = static_cast<int64_t>(ftello(file));
#endif
    if (size <= 0) {
        std::fclose(file);
        error = "empty file";
        return false;
    }
    if (std::fseek(file, 0, SEEK_SET) != 0) {
        std::fclose(file);
        error = "failed to seek input";
        return false;
    }
    out.resize(static_cast<size_t>(size));
    const size_t got = std::fread(out.data(), 1, out.size(), file);
    std::fclose(file);
    if (got != out.size()) {
        error = "failed to read input";
        return false;
    }
    return true;
}

struct PtsMark {
    size_t es_offset = 0;
    int64_t pts = -1;
};

}  // namespace

bool MpegPsDemuxer::open(const std::filesystem::path& path, std::string& error) {
    std::vector<uint8_t> file;
    if (!readWholeFile(path, file, error)) {
        return false;
    }

    std::vector<PtsMark> marks;
    size_t pos = 0;
    bool saw_pack = false;

    while (pos + 4 <= file.size()) {
        MpegStartCode sc;
        if (!findMpegStartCode(file, pos, sc)) {
            break;
        }
        const uint8_t code = sc.code;
        const size_t at = static_cast<size_t>(sc.body);

        if (isElementaryStartCode(code)) {
            error = "the file is a raw elementary stream, not a program stream";
            return false;
        }

        if (code == kPackStart) {
            saw_pack = true;
            if (at >= file.size()) {
                break;
            }
            // The top two bits distinguish the MPEG-2 pack header, which is
            // two bytes longer and carries a stuffing count.
            if ((file[at] >> 6) == 1) {
                if (at + 9 >= file.size()) {
                    break;
                }
                // Ten bytes follow the start code, the last of which holds the
                // three bit stuffing count.
                pos = at + 10 + (file[at + 9] & 0x07);
            } else {
                pos = at + 8;
            }
            continue;
        }

        if (code == kSystemHeader || code == kPaddingStream || code == kPrivateStream2 ||
            code == kProgramStreamMap) {
            if (at + 2 > file.size()) {
                break;
            }
            pos = at + 2 + readBe16(file, at);
            continue;
        }

        if (code == kProgramEnd) {
            break;
        }

        if (isVideoStreamId(code) || isAudioStreamId(code)) {
            if (at + 2 > file.size()) {
                break;
            }
            const int64_t packet_length = readBe16(file, at);
            size_t payload_start = at + 2;
            int64_t pts = -1;
            int64_t payload = 0;
            if (!parsePesHeader(file, payload_start, packet_length, pts, payload)) {
                pos = at + 2;
                continue;
            }

            if (payload == kUnbounded) {
                size_t end = file.size();
                findSystemStartCode(file, payload_start, end);
                payload = static_cast<int64_t>(end) - static_cast<int64_t>(payload_start);
            }
            const size_t available = file.size() - std::min(payload_start, file.size());
            const size_t count = std::min(static_cast<size_t>(std::max<int64_t>(payload, 0)),
                                          available);

            if (isVideoStreamId(code)) {
                marks.push_back({video_es_.size(), pts});
                video_es_.insert(video_es_.end(), file.begin() + static_cast<ptrdiff_t>(payload_start),
                              file.begin() + static_cast<ptrdiff_t>(payload_start + count));
            }
            pos = payload_start + count;
            continue;
        }

        // Unknown system code: skip its declared length if it has one.
        if (at + 2 <= file.size()) {
            pos = at + 2 + readBe16(file, at);
        } else {
            break;
        }
    }

    if (!saw_pack) {
        error = "no pack header found in program stream";
        return false;
    }
    if (video_es_.empty()) {
        error = "no video stream found in program stream";
        return false;
    }

    MpegEsInfo info;
    scanMpegPictures(video_es_, static_cast<int64_t>(video_es_.size()), pictures_, info);
    if (pictures_.empty()) {
        error = "no MPEG pictures found in program stream";
        return false;
    }

    // Attach each picture to the PTS of the PES packet it started in.
    for (MpegEsPicture& picture : pictures_) {
        for (const PtsMark& mark : marks) {
            if (static_cast<int64_t>(mark.es_offset) <= picture.offset && mark.pts >= 0) {
                picture.pts_ms = mark.pts / 90;  // PTS is in 90 kHz units
                picture.has_pts = true;
            }
        }
    }

    // The sequence header states the frame rate; the PTS spacing is only a
    // fallback for streams whose header is missing or reserved.
    fps_ = info.fps > 0.0 ? info.fps : 25.0;
    if (info.fps <= 0.0) {
        for (size_t i = 1; i < pictures_.size(); ++i) {
            if (pictures_[i].has_pts && pictures_[i - 1].has_pts) {
                const int64_t delta = pictures_[i].pts_ms - pictures_[i - 1].pts_ms;
                if (delta > 0 && delta <= 5000) {
                    fps_ = 1000.0 / static_cast<double>(delta);
                    break;
                }
            }
        }
    }

    video_ = VideoTrackInfo{};
    video_.codec = info.mpeg2 ? Codec::Mpeg2 : Codec::Mpeg1;
    video_.width = info.width;
    video_.height = info.height;
    video_.fps = fps_;
    video_.timescale = 1000;
    video_.format.annexb = true;
    next_picture_ = 0;
    return true;
}

double MpegPsDemuxer::duration() const {
    if (pictures_.empty()) {
        return -1.0;
    }
    if (pictures_.back().has_pts && pictures_.front().has_pts) {
        const double span =
            static_cast<double>(pictures_.back().pts_ms - pictures_.front().pts_ms) / 1000.0;
        return span + 1.0 / fps_;
    }
    return static_cast<double>(pictures_.size()) / fps_;
}

bool MpegPsDemuxer::readSample(Sample& sample) {
    if (next_picture_ >= pictures_.size()) {
        return false;
    }
    const MpegEsPicture& picture = pictures_[next_picture_];
    const size_t offset = static_cast<size_t>(picture.offset);
    const size_t size = static_cast<size_t>(picture.size);
    if (offset + size > video_es_.size()) {
        return false;
    }
    sample.data.assign(video_es_.begin() + static_cast<ptrdiff_t>(offset),
                       video_es_.begin() + static_cast<ptrdiff_t>(offset + size));

    int64_t pts = 0;
    if (picture.has_pts) {
        pts = picture.pts_ms;
    } else {
        pts = static_cast<int64_t>(
            std::llround(static_cast<double>(next_picture_) * 1000.0 / fps_));
    }
    sample.pts = pts;
    sample.dts = pts;
    sample.keyframe = picture.type == 1;
    sample.track = TrackType::Video;
    ++next_picture_;
    return true;
}

bool MpegPsDemuxer::seek(double seconds) {
    if (pictures_.empty()) {
        return false;
    }
    if (seconds < 0.0) {
        seconds = 0.0;
    }

    size_t target = 0;
    if (pictures_.front().has_pts) {
        const int64_t target_ms =
            pictures_.front().pts_ms + static_cast<int64_t>(seconds * 1000.0);
        target = pictures_.size() - 1;
        for (size_t i = 0; i < pictures_.size(); ++i) {
            if (pictures_[i].has_pts && pictures_[i].pts_ms > target_ms) {
                target = i > 0 ? i - 1 : 0;
                break;
            }
        }
    } else {
        target = static_cast<size_t>(seconds * fps_);
        if (target >= pictures_.size()) {
            target = pictures_.size() - 1;
        }
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
