#include "demux/FlvDemuxer.hpp"

#include "demux/Avcc.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace player::demux {
namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(a) << 24) | (static_cast<uint32_t>(b) << 16) |
           (static_cast<uint32_t>(c) << 8) | static_cast<uint32_t>(d);
}

uint32_t readU24(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 16) | (static_cast<uint32_t>(data[1]) << 8) |
           static_cast<uint32_t>(data[2]);
}

uint32_t readU32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

// The values onMetaData carries that this demuxer uses.
struct Metadata {
    bool have_duration = false;
    bool have_width = false;
    bool have_height = false;
    bool have_fps = false;
    double duration = 0.0;
    double width = 0.0;
    double height = 0.0;
    double fps = 0.0;
};

// Minimal AMF0 reader: enough to walk onMetaData and skip anything else
// without understanding it.
class AmfReader {
public:
    AmfReader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool ok() const { return ok_; }

    uint8_t u8() {
        if (pos_ >= size_) {
            ok_ = false;
            return 0;
        }
        return data_[pos_++];
    }

    uint16_t u16() {
        const uint16_t high = u8();
        return static_cast<uint16_t>((high << 8) | u8());
    }

    uint32_t u32() {
        const uint32_t a = u8();
        const uint32_t b = u8();
        const uint32_t c = u8();
        const uint32_t d = u8();
        return (a << 24) | (b << 16) | (c << 8) | d;
    }

    double number() {
        const uint64_t bits = (static_cast<uint64_t>(u32()) << 32) | u32();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    bool skip(size_t count) {
        if (pos_ + count > size_) {
            ok_ = false;
            return false;
        }
        pos_ += count;
        return true;
    }

    bool matchString(const char* text) {
        const size_t length = std::strlen(text);
        if (pos_ + length > size_ || std::memcmp(data_ + pos_, text, length) != 0) {
            return false;
        }
        pos_ += length;
        return true;
    }

    bool skipValue() {
        const uint8_t marker = u8();
        return skipPayload(marker, 0);
    }

    bool readMetadata(Metadata& metadata) {
        const uint8_t marker = u8();
        if (marker != 0x02) {
            return skipPayload(marker, 0);
        }
        const uint16_t length = u16();
        if (!matchString("onMetaData")) {
            return skip(length);
        }
        const uint8_t value_marker = u8();
        if (value_marker == 0x08) {
            (void)u32();  // array count, unreliable in practice
            return readObject(metadata, 0);
        }
        if (value_marker == 0x03) {
            return readObject(metadata, 0);
        }
        return skipPayload(value_marker, 0);
    }

private:
    bool readObject(Metadata& metadata, int depth) {
        if (depth > 8) {
            return false;
        }
        for (;;) {
            const uint16_t key_length = u16();
            if (!ok_) {
                return false;
            }
            if (key_length == 0) {
                return u8() == 0x09;
            }
            if (pos_ + key_length > size_) {
                ok_ = false;
                return false;
            }
            const char* key = reinterpret_cast<const char*>(data_ + pos_);
            pos_ += key_length;
            const uint8_t marker = u8();
            if (marker == 0x00) {
                const double value = number();
                const std::string name(key, key_length);
                if (name == "duration") {
                    metadata.have_duration = true;
                    metadata.duration = value;
                } else if (name == "width") {
                    metadata.have_width = true;
                    metadata.width = value;
                } else if (name == "height") {
                    metadata.have_height = true;
                    metadata.height = value;
                } else if (name == "framerate") {
                    metadata.have_fps = true;
                    metadata.fps = value;
                }
            } else if (marker == 0x01) {
                (void)u8();
            } else if (!skipPayload(marker, depth + 1)) {
                return false;
            }
        }
    }

    bool skipPayload(uint8_t marker, int depth) {
        if (depth > 8) {
            return false;
        }
        switch (marker) {
            case 0x00:
                return skip(8);
            case 0x01:
                return skip(1);
            case 0x02: {
                const uint16_t length = u16();
                return skip(length);
            }
            case 0x03: {
                Metadata ignored;
                return readObject(ignored, depth + 1);
            }
            case 0x05:
            case 0x06:
                return true;
            case 0x07:
                return skip(2);
            case 0x08:
                (void)u32();
                {
                    Metadata ignored;
                    return readObject(ignored, depth + 1);
                }
            case 0x09:
                return true;
            case 0x0A: {
                const uint32_t count = u32();
                for (uint32_t i = 0; i < count && ok_; ++i) {
                    if (!skipValue()) {
                        return false;
                    }
                }
                return ok_;
            }
            case 0x0B:
                return skip(10);
            case 0x0C: {
                const uint32_t length = u32();
                return skip(length);
            }
            case 0x0F: {
                const uint32_t length = u32();
                return skip(length);
            }
            case 0x10: {
                const uint16_t length = u16();
                Metadata ignored;
                return skip(length) && readObject(ignored, depth + 1);
            }
            default:
                return false;
        }
    }

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t pos_ = 0;
    bool ok_ = true;
};

// The FLV1 picture header states the coded picture size; the onMetaData tag
// usually repeats it, but not always.
bool parseFlv1Dimensions(const uint8_t* data, size_t size, uint32_t& width, uint32_t& height) {
    size_t bit = 0;
    const auto read = [&](unsigned count, uint32_t& out) {
        if (bit + count > size * 8) {
            return false;
        }
        uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            value = (value << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1u);
            ++bit;
        }
        out = value;
        return true;
    };
    uint32_t start = 0;
    uint32_t format = 0;
    uint32_t timestamp = 0;
    uint32_t size_code = 0;
    if (!read(17, start) || start != 1 || !read(5, format) || !read(8, timestamp) ||
        !read(3, size_code)) {
        return false;
    }
    (void)format;
    (void)timestamp;
    switch (size_code) {
        case 0:
            return read(8, width) && read(8, height);
        case 1:
            return read(16, width) && read(16, height);
        case 2:
            width = 352;
            height = 288;
            return true;
        case 3:
            width = 176;
            height = 144;
            return true;
        case 4:
            width = 128;
            height = 96;
            return true;
        case 5:
            width = 320;
            height = 240;
            return true;
        case 6:
            width = 160;
            height = 120;
            return true;
        default:
            return false;
    }
}

bool parseAudioSpecificConfig(const uint8_t* data, size_t size, uint32_t& sample_rate,
                              uint32_t& channels) {
    static const uint32_t kSampleRates[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                              22050, 16000, 12000, 11025, 8000, 7350};
    size_t bit = 0;
    const auto read = [&](unsigned count, uint32_t& out) {
        if (bit + count > size * 8) {
            return false;
        }
        uint32_t value = 0;
        for (unsigned i = 0; i < count; ++i) {
            value = (value << 1) | ((data[bit >> 3] >> (7 - (bit & 7))) & 1u);
            ++bit;
        }
        out = value;
        return true;
    };
    uint32_t object_type = 0;
    uint32_t frequency_index = 0;
    uint32_t channel_config = 0;
    if (!read(5, object_type) || !read(4, frequency_index) || !read(4, channel_config)) {
        return false;
    }
    if (object_type == 31) {
        uint32_t extension = 0;
        if (!read(6, extension)) {
            return false;
        }
    }
    if (frequency_index == 15) {
        uint32_t explicit_rate = 0;
        if (!read(24, explicit_rate)) {
            return false;
        }
        sample_rate = explicit_rate;
    } else if (frequency_index < 13) {
        sample_rate = kSampleRates[frequency_index];
    } else {
        return false;
    }
    channels = channel_config;
    return sample_rate != 0 && channels != 0;
}

const char* audioCodecName(uint8_t sound_format) {
    switch (sound_format) {
        case 0:
            return "pcm";
        case 1:
            return "adpcm";
        case 2:
            return "mp3";
        case 3:
            return "pcm_le";
        case 4:
            return "nellymoser16";
        case 5:
            return "nellymoser8";
        case 6:
            return "nellymoser";
        case 7:
            return "alaw";
        case 8:
            return "mulaw";
        case 10:
            return "aac";
        case 11:
            return "speex";
        default:
            return "unknown";
    }
}

}  // namespace

FlvDemuxer::FlvDemuxer() = default;

FlvDemuxer::~FlvDemuxer() {
    if (file_) {
        std::fclose(file_);
    }
}

bool FlvDemuxer::readAt(int64_t offset, void* data, size_t size) {
    if (!file_ || offset < 0 || offset + static_cast<int64_t>(size) > file_size_) {
        return false;
    }
    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
        return false;
    }
    return std::fread(data, 1, size, file_) == size;
}

bool FlvDemuxer::readPayload(const TagInfo& tag, std::vector<uint8_t>& data) {
    data.resize(tag.size);
    return tag.size == 0 || readAt(tag.offset + 11, data.data(), tag.size);
}

bool FlvDemuxer::open(const std::filesystem::path& path, std::string& error) {
    path_ = path;
    file_ = std::fopen(path.string().c_str(), "rb");
    if (!file_) {
        error = "cannot open " + path.string();
        return false;
    }
    std::fseek(file_, 0, SEEK_END);
    file_size_ = std::ftell(file_);
    std::fseek(file_, 0, SEEK_SET);

    video_tags_.clear();
    audio_tags_.clear();
    next_video_ = 0;
    next_audio_ = 0;
    if (!scanTags(error)) {
        std::fclose(file_);
        file_ = nullptr;
        return false;
    }
    return true;
}

bool FlvDemuxer::scanTags(std::string& error) {
    uint8_t header[9] = {};
    if (!readAt(0, header, sizeof(header)) || std::memcmp(header, "FLV", 3) != 0) {
        error = "not an FLV file";
        return false;
    }
    uint32_t data_offset = readU32(header + 5);
    if (data_offset < 9) {
        data_offset = 9;
    }

    Metadata metadata;
    bool have_video = false;
    bool have_audio = false;
    int64_t pos = data_offset;
    uint8_t previous[4];
    if (pos + 4 > file_size_ || !readAt(pos, previous, 4)) {
        error = "truncated FLV header";
        return false;
    }
    pos += 4;

    uint8_t tag[11];
    while (pos + 11 <= file_size_) {
        if (!readAt(pos, tag, sizeof(tag))) {
            break;
        }
        const uint8_t type = tag[0];
        const uint32_t size = readU24(tag + 1);
        const int64_t timestamp =
            static_cast<int64_t>(readU24(tag + 4)) | (static_cast<int64_t>(tag[7]) << 24);
        const int64_t data_offset_tag = pos + 11;
        if (data_offset_tag + size > file_size_) {
            break;
        }

        if (type == 9 && size >= 1) {
            uint8_t first = 0;
            if (!readAt(data_offset_tag, &first, 1)) {
                break;
            }
            const uint8_t codec_id = first & 0x0F;
            const bool keyframe = (first >> 4) == 1;
            if (!have_video) {
                have_video = true;
                if (codec_id == 2) {
                    video_.codec = Codec::Flv1;
                    video_.codec_name = "FLV1";
                } else if (codec_id == 7) {
                    video_.codec = Codec::H264;
                    video_.codec_name = "H.264";
                    video_.format.annexb = false;
                    video_.format.nal_length_size = 4;
                } else if (codec_id == 4 || codec_id == 5) {
                    video_.codec_name = "On2 VP6";
                } else if (codec_id == 3 || codec_id == 6) {
                    video_.codec_name = "Screen video";
                } else {
                    video_.codec_name = "unknown codec id " + std::to_string(codec_id);
                }
            }
            // A video codec without a decoder is not indexed: the file still
            // plays its audio. H.264 tags are indexed except for the sequence
            // header, which carries the avcC record.
            if (video_.codec == Codec::H264 && size >= 5) {
                uint8_t packet_type = 0;
                if (readAt(data_offset_tag + 1, &packet_type, 1)) {
                    if (packet_type == 0) {
                        // The FLV video header, AVCPacketType and composition
                        // time precede the avcC record.
                        std::vector<uint8_t> config;
                        config.resize(size - 5);
                        if (readAt(data_offset_tag + 5, config.data(), config.size())) {
                            video_.avcc = std::move(config);
                            AvccInfo avcc;
                            if (parseAvcc(video_.avcc.data(), video_.avcc.size(), avcc, nullptr)) {
                                video_.format.nal_length_size = avcc.length_size;
                            }
                        }
                    } else if (packet_type == 1) {
                        TagInfo info;
                        info.offset = pos;
                        info.size = size;
                        info.pts_ms = timestamp;
                        info.keyframe = keyframe;
                        video_tags_.push_back(info);
                    }
                }
            } else if (video_.codec == Codec::Flv1) {
                TagInfo info;
                info.offset = pos;
                info.size = size;
                info.pts_ms = timestamp;
                info.keyframe = keyframe;
                video_tags_.push_back(info);
            }
        } else if (type == 8 && size >= 1) {
            uint8_t first = 0;
            if (!readAt(data_offset_tag, &first, 1)) {
                break;
            }
            const uint8_t sound_format = first >> 4;
            if (!have_audio) {
                have_audio = true;
                std::vector<uint8_t> payload;
                payload.resize(size);
                if (readAt(data_offset_tag, payload.data(), size)) {
                    configureAudio(sound_format, payload.data(), payload.size());
                }
            }
            // An AAC sequence header configures the decoder; it is not a
            // sample and must not reach the audio buffer.
            bool is_config = false;
            if (sound_format == 10 && size >= 2) {
                uint8_t packet_type = 0;
                if (readAt(data_offset_tag + 1, &packet_type, 1) && packet_type == 0) {
                    is_config = true;
                }
            }
            if (!is_config) {
                TagInfo info;
                info.offset = pos;
                info.size = size;
                info.pts_ms = timestamp;
                audio_tags_.push_back(info);
            }
        } else if (type == 18 && size > 0) {
            std::vector<uint8_t> payload;
            payload.resize(size);
            if (readAt(data_offset_tag, payload.data(), size)) {
                AmfReader reader(payload.data(), payload.size());
                reader.readMetadata(metadata);
            }
        }

        const int64_t next = data_offset_tag + size + 4;
        if (next <= pos) {
            break;
        }
        pos = next;
    }

    if (!have_video && !have_audio) {
        error = "FLV file has no audio or video tags";
        return false;
    }
    if (have_video && video_.codec == Codec::Unknown && !have_audio) {
        error = "FLV: " + video_.codec_name + " video is not supported";
        return false;
    }

    if (metadata.have_duration && metadata.duration > 0.0) {
        duration_ = metadata.duration;
        have_duration_ = true;
    }
    if (metadata.have_width && metadata.have_height && metadata.width > 0.0 &&
        metadata.height > 0.0) {
        video_.width = static_cast<uint32_t>(metadata.width);
        video_.height = static_cast<uint32_t>(metadata.height);
    }
    if (metadata.have_fps && metadata.fps > 0.0) {
        video_.fps = metadata.fps;
        have_fps_ = true;
    }
    if (have_video && !video_tags_.empty()) {
        const TagInfo& first = video_tags_.front();
        if (video_.width == 0 || video_.height == 0) {
            std::vector<uint8_t> payload;
            if (readPayload(first, payload) && payload.size() > 1) {
                (void)parseFlv1Dimensions(payload.data() + 1, payload.size() - 1, video_.width,
                                          video_.height);
            }
        }
    }
    finishTracks();
    return true;
}

void FlvDemuxer::configureAudio(uint8_t sound_format, const uint8_t* payload, size_t size) {
    static const uint32_t kRates[4] = {5512, 11025, 22050, 44100};
    audio_.index = 0;
    audio_.present = true;
    audio_.default_track = true;
    audio_.language = "und";
    audio_.codec = audioCodecName(sound_format);
    audio_.name = audio_.codec;
    audio_.channels = (payload[0] & 1) != 0 ? 2 : 1;
    audio_.sample_rate = kRates[(payload[0] >> 2) & 3];
    switch (sound_format) {
        case 2:
            audio_.codec_fourcc = fourcc('m', 'p', '3', ' ');
            break;
        case 10: {
            audio_.codec_fourcc = fourcc('m', 'p', '4', 'a');
            if (size >= 2) {
                uint32_t rate = 0;
                uint32_t channels = 0;
                if (parseAudioSpecificConfig(payload + 2, size - 2, rate, channels)) {
                    audio_.sample_rate = rate;
                    audio_.channels = channels;
                }
                // The sequence header is the decoder configuration; keep it
                // for the engine the way other containers keep extradata.
                audio_.extradata.assign(payload + 2, payload + size);
            }
            break;
        }
        default:
            audio_.codec_fourcc = 0;
            break;
    }
}

void FlvDemuxer::finishTracks() {
    video_.timescale = 1000;
    int64_t last_pts = 0;
    if (!video_tags_.empty()) {
        const int64_t first_pts = video_tags_.front().pts_ms;
        last_pts = video_tags_.back().pts_ms;
        if (!have_fps_ && video_tags_.size() > 1 && last_pts > first_pts) {
            video_.fps = static_cast<double>(video_tags_.size() - 1) * 1000.0 /
                         static_cast<double>(last_pts - first_pts);
            have_fps_ = true;
        }
    }
    if (!audio_tags_.empty()) {
        last_pts = std::max(last_pts, audio_tags_.back().pts_ms);
    }
    if (!have_duration_) {
        duration_ = static_cast<double>(last_pts) / 1000.0;
        if (video_.fps > 0.0) {
            duration_ += 1.0 / video_.fps;
        }
    }
}

std::vector<AudioTrackInfo> FlvDemuxer::audioTracks() const {
    if (!audio_.present) {
        return {};
    }
    return {audio_};
}

bool FlvDemuxer::selectAudioTrack(size_t index) {
    return index == 0 && audio_.present;
}

bool FlvDemuxer::readSample(Sample& sample) {
    while (next_video_ < video_tags_.size()) {
        const TagInfo& tag = video_tags_[next_video_++];
        if (!readPayload(tag, sample.data) || sample.data.empty()) {
            continue;
        }
        sample.data.erase(sample.data.begin());  // the FLV frame-type/codec byte
        if (video_.codec == Codec::H264) {
            // The AVCPacketType byte and the three composition time bytes
            // follow the FLV video header; the decoder wants the NAL units.
            if (sample.data.size() <= 4) {
                continue;
            }
            sample.data.erase(sample.data.begin(), sample.data.begin() + 4);
        }
        if (sample.data.empty()) {
            continue;
        }
        sample.pts = tag.pts_ms;
        sample.dts = tag.pts_ms;
        sample.keyframe = tag.keyframe;
        sample.track = TrackType::Video;
        return true;
    }
    return false;
}

bool FlvDemuxer::readAudioSample(Sample& sample) {
    while (next_audio_ < audio_tags_.size()) {
        const TagInfo& tag = audio_tags_[next_audio_++];
        if (!readPayload(tag, sample.data) || sample.data.empty()) {
            continue;
        }
        sample.data.erase(sample.data.begin());  // the FLV sound-format byte
        // An AAC tag carries its packet type (raw frame) in the next byte;
        // the decoder wants the access unit alone.
        if (audio_.codec_fourcc == fourcc('m', 'p', '4', 'a') && !sample.data.empty()) {
            sample.data.erase(sample.data.begin());
        }
        sample.pts = tag.pts_ms;
        sample.dts = tag.pts_ms;
        sample.keyframe = true;
        sample.track = TrackType::Audio;
        return true;
    }
    return false;
}

bool FlvDemuxer::seek(double seconds) {
    const int64_t target_ms = static_cast<int64_t>(seconds * 1000.0);
    bool found = false;

    // Last keyframe at or before the target, so a seek decodes forward from a
    // clean reference.
    size_t video_index = 0;
    for (size_t i = 0; i < video_tags_.size(); ++i) {
        if (video_tags_[i].pts_ms > target_ms) {
            break;
        }
        if (video_tags_[i].keyframe) {
            video_index = i;
            found = true;
        }
    }
    next_video_ = video_index;

    const auto it = std::lower_bound(audio_tags_.begin(), audio_tags_.end(), target_ms,
                                     [](const TagInfo& tag, int64_t value) {
                                         return tag.pts_ms < value;
                                     });
    // Step back to the packet that covers the target for a short lead.
    next_audio_ = it == audio_tags_.begin()
                      ? 0
                      : static_cast<size_t>(std::prev(it) - audio_tags_.begin());
    return found || !audio_tags_.empty();
}

}  // namespace player::demux
