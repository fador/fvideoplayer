#include "demux/RawHevcDemuxer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace player::demux {
namespace {

struct NalPos {
    int64_t offset = 0;
    uint8_t type = 0;
    bool first_slice = true;
};

bool isVcl(uint8_t type) {
    return type <= 31;
}

bool seekFile(FILE* file, int64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, offset, SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

bool isIrap(uint8_t type) {
    return type >= 16 && type <= 23;
}

// Scans an Annex B stream for NAL units. Records the absolute file offset of
// each NAL header, its type, and first_slice_segment_in_pic_flag for VCL NALs.
std::vector<NalPos> scanNals(FILE* file, std::string& error) {
    std::vector<NalPos> nals;
    constexpr size_t kChunk = 1 << 16;
    std::vector<uint8_t> buffer(kChunk + 8);

    if (std::fseek(file, 0, SEEK_SET) != 0) {
        error = "failed to seek input";
        return nals;
    }

    int64_t base = 0;
    size_t carry = 0;
    for (;;) {
        const size_t got = std::fread(buffer.data() + carry, 1, kChunk, file);
        const size_t total = carry + got;

        size_t i = 0;
        while (i + 6 <= total) {
            if (buffer[i] == 0 && buffer[i + 1] == 0 && buffer[i + 2] == 1) {
                NalPos nal;
                nal.type = static_cast<uint8_t>(buffer[i + 3] >> 1);
                nal.offset = base + static_cast<int64_t>(i) + 3;
                if (isVcl(nal.type)) {
                    nal.first_slice = (buffer[i + 5] & 0x80) != 0;
                }
                nals.push_back(nal);
                i += 3;
            } else {
                ++i;
            }
        }

        if (got == 0) {
            break;
        }
        const size_t consumed = i;
        carry = total - consumed;
        std::memmove(buffer.data(), buffer.data() + consumed, carry);
        base += static_cast<int64_t>(consumed);
    }
    return nals;
}

std::vector<RawHevcDemuxer::AccessUnit> buildAccessUnits(const std::vector<NalPos>& nals,
                                                         int64_t file_size) {
    std::vector<RawHevcDemuxer::AccessUnit> units;
    int64_t au_start = -1;
    int64_t pending_start = -1;
    bool seen_vcl = false;
    bool au_has_irap = false;

    auto flush = [&](int64_t end) {
        if (au_start >= 0 && end > au_start) {
            units.push_back({au_start, end - au_start, au_has_irap});
        }
    };

    for (const NalPos& nal : nals) {
        if (isVcl(nal.type)) {
            if (seen_vcl && nal.first_slice) {
                const int64_t end = pending_start >= 0 ? pending_start : nal.offset;
                flush(end);
                au_start = pending_start >= 0 ? pending_start : nal.offset;
                pending_start = -1;
                au_has_irap = false;
            }
            if (au_start < 0) {
                au_start = nal.offset;
            }
            seen_vcl = true;
            au_has_irap = au_has_irap || isIrap(nal.type);
        } else {
            if (!seen_vcl) {
                if (au_start < 0) {
                    au_start = nal.offset;
                }
            } else if (pending_start < 0) {
                pending_start = nal.offset;
            }
        }
    }
    if (au_start >= 0) {
        flush(file_size);
    }
    return units;
}

}  // namespace

RawHevcDemuxer::RawHevcDemuxer(double fps) : fps_(fps > 0.0 ? fps : 25.0) {}

RawHevcDemuxer::~RawHevcDemuxer() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool RawHevcDemuxer::open(const std::filesystem::path& path, std::string& error) {
    file_ = std::fopen(path.string().c_str(), "rb");
    if (!file_) {
        error = "failed to open " + path.string();
        return false;
    }
    if (std::fseek(file_, 0, SEEK_END) != 0) {
        error = "failed to determine file size";
        return false;
    }
#ifdef _WIN32
    file_size_ = _ftelli64(file_);
#else
    file_size_ = static_cast<int64_t>(ftello(file_));
#endif

    const std::vector<NalPos> nals = scanNals(file_, error);
    if (!error.empty()) {
        return false;
    }
    units_ = buildAccessUnits(nals, file_size_);
    if (units_.empty()) {
        error = "no HEVC pictures found in stream";
        return false;
    }

    video_ = VideoTrackInfo{};
    video_.codec = Codec::Hevc;
    video_.fps = fps_;
    video_.timescale = 1000;
    video_.format.annexb = true;

    next_unit_ = 0;
    return true;
}

double RawHevcDemuxer::duration() const {
    if (units_.empty()) {
        return -1.0;
    }
    return static_cast<double>(units_.size()) / fps_;
}

bool RawHevcDemuxer::readSample(Sample& sample) {
    if (!file_ || next_unit_ >= units_.size()) {
        return false;
    }
    const AccessUnit& unit = units_[next_unit_];

    sample.data.resize(static_cast<size_t>(unit.size));
    if (!seekFile(file_, unit.offset)) {
        return false;
    }
    if (std::fread(sample.data.data(), 1, sample.data.size(), file_) != sample.data.size()) {
        return false;
    }

    const int64_t pts = static_cast<int64_t>(
        std::llround(static_cast<double>(next_unit_) * 1000.0 / fps_));
    sample.pts = pts;
    sample.dts = pts;
    sample.keyframe = unit.keyframe;
    sample.track = TrackType::Video;
    ++next_unit_;
    return true;
}

bool RawHevcDemuxer::seek(double seconds) {
    if (units_.empty()) {
        return false;
    }
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    size_t target = static_cast<size_t>(seconds * fps_);
    if (target >= units_.size()) {
        target = units_.size() - 1;
    }

    size_t index = 0;
    for (size_t i = target + 1; i-- > 0;) {
        if (units_[i].keyframe) {
            index = i;
            break;
        }
    }
    next_unit_ = index;
    return true;
}

}  // namespace player::demux
