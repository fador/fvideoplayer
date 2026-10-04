#include "demux/RawMpegDemuxer.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace player::demux {
namespace {

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

bool readWholeFile(FILE* file, std::vector<uint8_t>& out, std::string& error) {
    if (std::fseek(file, 0, SEEK_END) != 0) {
        error = "failed to determine file size";
        return false;
    }
    const int64_t size = fileSize(file);
    if (size <= 0) {
        error = "empty file";
        return false;
    }
    if (!seekFile(file, 0)) {
        error = "failed to seek input";
        return false;
    }
    out.resize(static_cast<size_t>(size));
    if (std::fread(out.data(), 1, out.size(), file) != out.size()) {
        error = "failed to read input";
        return false;
    }
    return true;
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
    if (file_ == nullptr) {
        error = "failed to open " + path.string();
        return false;
    }

    std::vector<uint8_t> buffer;
    if (!readWholeFile(file_, buffer, error)) {
        return false;
    }

    MpegEsInfo info;
    scanMpegPictures(buffer, static_cast<int64_t>(buffer.size()), pictures_, info);
    if (pictures_.empty()) {
        error = "no MPEG pictures found in stream";
        return false;
    }

    video_ = VideoTrackInfo{};
    video_.codec = info.mpeg2 ? Codec::Mpeg2 : Codec::Mpeg1;
    video_.width = info.width;
    video_.height = info.height;
    // The sequence header knows the real frame rate; only fall back to the
    // constructor's guess when the stream never carried one.
    if (info.fps > 0.0) {
        fps_ = info.fps;
    }
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
    if (file_ == nullptr || next_picture_ >= pictures_.size()) {
        return false;
    }
    const MpegEsPicture& picture = pictures_[next_picture_];
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
