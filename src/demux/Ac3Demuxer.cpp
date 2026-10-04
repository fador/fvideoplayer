#include "demux/Ac3Demuxer.hpp"

#include <cstring>

namespace player::demux {
namespace {

// ATSC A/52 Table 5.18: frame size in 16-bit words for 48, 44.1 and 32 kHz.
const uint16_t kFrameSize[38][3] = {
    {64, 69, 96},       {64, 70, 96},       {80, 87, 120},      {80, 88, 120},
    {96, 104, 144},     {96, 105, 144},     {112, 121, 168},    {112, 122, 168},
    {128, 139, 192},    {128, 140, 192},    {160, 174, 240},    {160, 175, 240},
    {192, 208, 288},    {192, 209, 288},    {224, 243, 336},    {224, 244, 336},
    {256, 278, 384},    {256, 279, 384},    {320, 348, 480},    {320, 349, 480},
    {384, 417, 576},    {384, 418, 576},    {448, 487, 672},    {448, 488, 672},
    {512, 557, 768},    {512, 558, 768},    {640, 696, 960},    {640, 697, 960},
    {768, 835, 1152},   {768, 836, 1152},   {896, 975, 1344},   {896, 976, 1344},
    {1024, 1114, 1536}, {1024, 1115, 1536}, {1152, 1253, 1728}, {1152, 1254, 1728},
    {1280, 1393, 1920}, {1280, 1394, 1920},
};

const int kSampleRates[3] = {48000, 44100, 32000};
const uint8_t kChannelsTab[8] = {2, 1, 2, 3, 3, 4, 4, 5};

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

}  // namespace

uint32_t Ac3Demuxer::frameSize(const uint8_t* data, size_t size) {
    if (size < 8 || data == nullptr || readU16(data) != 0x0B77) {
        return 0;
    }
    const uint8_t fscod = data[4] >> 6;
    const uint8_t frame_size_code = data[4] & 0x3F;
    const uint8_t bitstream_id = (data[5] >> 3) & 0x1F;
    if (fscod == 3 || frame_size_code > 37 || bitstream_id > 10) {
        return 0;
    }
    return kFrameSize[frame_size_code][fscod] * 2u;
}

Ac3Demuxer::Ac3Demuxer() = default;

Ac3Demuxer::~Ac3Demuxer() {
    if (file_) {
        std::fclose(file_);
    }
}

bool Ac3Demuxer::readAt(int64_t offset, void* data, size_t size) {
    if (!file_ || offset < 0 || offset + static_cast<int64_t>(size) > file_size_) {
        return false;
    }
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }
    return std::fread(data, 1, size, file_) == size;
}

bool Ac3Demuxer::open(const std::filesystem::path& path, std::string& error) {
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

bool Ac3Demuxer::scanFrames(std::string& error) {
    uint8_t header[8] = {};
    int64_t offset = 0;
    int sample_rate = 0;
    while (offset + 8 <= file_size_) {
        if (!readAt(offset, header, sizeof(header))) {
            break;
        }
        const uint32_t size = frameSize(header, sizeof(header));
        if (size == 0) {
            if (frames_.empty()) {
                error = "no AC-3 syncframe found";
                return false;
            }
            break;
        }
        if (offset + static_cast<int64_t>(size) > file_size_) {
            break;
        }
        const uint8_t fscod = header[4] >> 6;
        const uint8_t bitstream_id = (header[5] >> 3) & 0x1F;
        const int sr_shift = (bitstream_id > 8 ? bitstream_id : 8) - 8;
        sample_rate = kSampleRates[fscod] >> sr_shift;
        const uint8_t channel_mode = (header[6] >> 5) & 7;
        int lfe_bit = 4;
        if (channel_mode == 2) {
            lfe_bit = 2;  // dsurmod
        } else {
            if ((channel_mode & 1) && channel_mode != 1) {
                lfe_bit -= 2;  // cmixlev
            }
            if (channel_mode & 4) {
                lfe_bit -= 2;  // surmixlev
            }
        }
        const bool lfe_on = ((header[6] >> lfe_bit) & 1) != 0;

        if (frames_.empty()) {
            audio_.index = 0;
            audio_.name = "AC-3";
            audio_.language = "und";
            audio_.codec = "ac-3";
            audio_.present = true;
            audio_.default_track = true;
            audio_.codec_fourcc = 0x61632D33;  // 'ac-3'
            audio_.channels = kChannelsTab[channel_mode] + (lfe_on ? 1 : 0);
            audio_.sample_rate = static_cast<uint32_t>(sample_rate);
        }

        FrameInfo info;
        info.offset = offset;
        info.size = size;
        info.pts_ms = static_cast<int64_t>(frames_.size()) * 1536 * 1000 / sample_rate;
        frames_.push_back(info);
        offset += size;
    }
    if (frames_.empty()) {
        error = "no AC-3 frames";
        return false;
    }
    video_.timescale = 1000;
    duration_ = static_cast<double>(frames_.size()) * 1536.0 / sample_rate;
    return true;
}

std::vector<AudioTrackInfo> Ac3Demuxer::audioTracks() const {
    if (!audio_.present) {
        return {};
    }
    return {audio_};
}

bool Ac3Demuxer::selectAudioTrack(size_t index) {
    return index == 0 && audio_.present;
}

bool Ac3Demuxer::readSample(Sample& sample) {
    (void)sample;
    return false;  // no video track
}

bool Ac3Demuxer::readAudioSample(Sample& sample) {
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

bool Ac3Demuxer::seek(double seconds) {
    if (frames_.empty()) {
        return false;
    }
    const int64_t target_ms = static_cast<int64_t>(seconds * 1000.0);
    size_t index = 0;
    for (size_t i = 0; i < frames_.size(); ++i) {
        if (frames_[i].pts_ms > target_ms) {
            break;
        }
        index = i;
    }
    next_audio_ = index;
    return true;
}

}  // namespace player::demux
