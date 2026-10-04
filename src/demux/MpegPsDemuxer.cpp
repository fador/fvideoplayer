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

// Names a video elementary stream that produced no MPEG pictures, for the
// "audio only" message. H.264 is recognised by its Annex B NAL units.
std::string detectVideoCodecName(const std::vector<uint8_t>& es) {
    for (size_t i = 0; i + 4 < es.size(); ++i) {
        if (es[i] != 0 || es[i + 1] != 0) {
            continue;
        }
        size_t nal = 0;
        if (es[i + 2] == 1) {
            nal = i + 3;
        } else if (es[i + 2] == 0 && es[i + 3] == 1) {
            nal = i + 4;
        } else {
            continue;
        }
        if (nal >= es.size()) {
            break;
        }
        const uint8_t type = es[nal] & 0x1F;
        if (type == 1 || type == 5 || type == 7 || type == 8 || type == 9) {
            return "H.264";
        }
    }
    return "unsupported video";
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
        // No optional fields follow, but the byte still belongs to the header:
        // the reference implementation consumes it rather than handing it to
        // the decoder as the first payload byte. Real MPEG-1 streams do use
        // this form, and keeping the byte shifts the whole elementary stream.
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

}  // namespace

bool MpegPsDemuxer::open(const std::filesystem::path& path, std::string& error) {
    std::vector<uint8_t> file;
    if (!readWholeFile(path, file, error)) {
        return false;
    }

    std::vector<PtsMark> marks;
    std::vector<PtsMark> audio_marks;
    size_t pos = 0;
    bool saw_pack = false;
    // Only one audio stream is kept: the first one seen.
    int audio_stream_id = -1;

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
            } else if (audio_stream_id < 0 || code == audio_stream_id) {
                audio_stream_id = code;
                audio_marks.push_back({audio_es_.size(), pts});
                audio_es_.insert(audio_es_.end(), file.begin() + static_cast<ptrdiff_t>(payload_start),
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

    scanAudioTrack(audio_marks);
    if (video_es_.empty() && !audio_.present) {
        error = "no video stream found in program stream";
        return false;
    }

    MpegEsInfo info;
    if (!video_es_.empty()) {
        scanMpegPictures(video_es_, static_cast<int64_t>(video_es_.size()), pictures_, info);
    }
    if (pictures_.empty()) {
        // The video is not MPEG-1/2 (H.264 in a program stream, for example):
        // keep the track description and play the audio.
        if (!audio_.present) {
            error = "no MPEG pictures found in program stream";
            return false;
        }
        video_ = VideoTrackInfo{};
        video_.codec_name = video_es_.empty() ? "" : detectVideoCodecName(video_es_);
        next_picture_ = 0;
        return true;
    }

    // A picture takes the PTS of the PES packet it started in. The packet's
    // time must not be inherited by a later picture whose own packet carries
    // none: that puts an early time on a later picture and the display order
    // stops being monotonic. A picture without its own PTS is marked as such
    // and placed on the timeline below.
    for (MpegEsPicture& picture : pictures_) {
        for (const PtsMark& mark : marks) {
            if (static_cast<int64_t>(mark.es_offset) == picture.offset && mark.pts >= 0) {
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

    // Place every picture on a frame-rate timeline indexed by display order,
    // anchored to the first time the stream states. Only some PES packets carry
    // a PTS, and the display order is what makes the timeline monotonic. A
    // stream that states a time for every picture keeps them as they are.
    bool every_picture_has_pts = !pictures_.empty();
    for (const MpegEsPicture& picture : pictures_) {
        if (!picture.has_pts) {
            every_picture_has_pts = false;
            break;
        }
    }
    if (!every_picture_has_pts) {
        const double frame_ms = 1000.0 / (fps_ > 0.0 ? fps_ : 25.0);
        int64_t anchor_ms = 0;
        int anchor_index = 0;
        for (const MpegEsPicture& picture : pictures_) {
            if (picture.has_pts) {
                anchor_ms = picture.pts_ms;
                anchor_index = picture.display_index;
                break;
            }
        }
        for (MpegEsPicture& picture : pictures_) {
            picture.pts_ms =
                anchor_ms +
                static_cast<int64_t>(
                    std::llround(static_cast<double>(picture.display_index - anchor_index) *
                                 frame_ms));
            picture.has_pts = true;
        }
    }

    video_ = VideoTrackInfo{};
    video_.codec = info.mpeg2 ? Codec::Mpeg2 : Codec::Mpeg1;
    video_.codec_name = info.mpeg2 ? "MPEG-2" : "MPEG-1";
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
        return audio_duration_ms_ > 0.0 ? audio_duration_ms_ / 1000.0 : -1.0;
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

void MpegPsDemuxer::scanAudioTrack(const std::vector<PtsMark>& marks) {
    audio_frames_.clear();
    if (audio_es_.empty()) {
        return;
    }
    static const uint32_t kBitrateMpeg1[15] = {0,   32,  48,  56,  64,  80,  96, 112,
                                               128, 160, 192, 224, 256, 320, 384};
    static const uint32_t kBitrateMpeg2[15] = {0,  8,  16, 24, 32,  40,  48,  56,
                                               64, 80, 96, 112, 128, 144, 160};
    static const uint32_t kRates[3][3] = {
        {44100, 48000, 32000}, {22050, 24000, 16000}, {11025, 12000, 8000}};

    size_t at = 0;
    int layer = 0;
    uint32_t rate = 0;
    int channels = 0;
    int samples_per_frame = 1152;
    while (at + 4 <= audio_es_.size()) {
        const uint8_t* h = audio_es_.data() + at;
        if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) {
            ++at;
            continue;
        }
        const int version_bits = (h[1] >> 3) & 3;
        const int layer_bits = (h[1] >> 1) & 3;
        const int bitrate_index = (h[2] >> 4) & 0x0F;
        const int rate_index = (h[2] >> 2) & 3;
        if (version_bits == 1 || layer_bits == 0 || bitrate_index == 0 ||
            bitrate_index == 15 || rate_index == 3) {
            ++at;
            continue;
        }
        const int version = version_bits == 3 ? 0 : (version_bits == 2 ? 1 : 2);
        const uint32_t bitrate =
            (version == 0 ? kBitrateMpeg1 : kBitrateMpeg2)[bitrate_index] * 1000u;
        const uint32_t frame_rate = kRates[version][rate_index];
        const int padding = (h[2] >> 1) & 1;
        // Layers II and III carry 1152 samples per frame in MPEG-1 and 576 in
        // MPEG-2 and 2.5.
        const int samples = version == 0 ? 1152 : 576;
        const int64_t frame_bytes =
            static_cast<int64_t>(samples / 8) * bitrate / frame_rate + padding;
        if (frame_bytes <= 4 || at + static_cast<size_t>(frame_bytes) > audio_es_.size()) {
            ++at;
            continue;
        }
        if (audio_frames_.empty()) {
            layer = layer_bits;
            rate = frame_rate;
            samples_per_frame = samples;
            channels = ((h[3] >> 6) & 3) == 3 ? 1 : 2;
        }
        audio_frames_.push_back({static_cast<int64_t>(at), frame_bytes, 0});
        at += static_cast<size_t>(frame_bytes);
    }
    if (audio_frames_.empty() || layer == 0 || rate == 0) {
        return;
    }

    audio_ = AudioTrackInfo{};
    audio_.present = true;
    audio_.default_track = true;
    audio_.channels = static_cast<uint32_t>(channels);
    audio_.sample_rate = rate;
    if (layer == 2) {
        audio_.codec = "mp2";
        audio_.codec_fourcc = 0x6D703220;  // 'mp2 '
    } else {
        audio_.codec = "mp3";
        audio_.codec_fourcc = 0x6D703320;  // 'mp3 '
    }

    // Place the frames on a frame-rate timeline anchored to the first time the
    // stream states. Only some PES packets carry a PTS, and stepping the frame
    // duration keeps the times monotonic where inheriting one would not.
    const double frame_ms = static_cast<double>(samples_per_frame) * 1000.0 / rate;
    int64_t anchor_ms = 0;
    int64_t anchor_index = 0;
    bool have_anchor = false;
    for (size_t i = 0; i < audio_frames_.size() && !have_anchor; ++i) {
        for (const PtsMark& mark : marks) {
            if (mark.pts >= 0 && mark.es_offset <= static_cast<size_t>(audio_frames_[i].offset)) {
                anchor_ms = mark.pts / 90;  // PTS is in 90 kHz units
                anchor_index = static_cast<int64_t>(i);
                have_anchor = true;
            }
        }
    }
    for (size_t i = 0; i < audio_frames_.size(); ++i) {
        audio_frames_[i].pts_ms =
            anchor_ms + static_cast<int64_t>(std::llround(
                            static_cast<double>(static_cast<int64_t>(i) - anchor_index) * frame_ms));
    }
    audio_duration_ms_ =
        static_cast<double>(audio_frames_.back().pts_ms) + frame_ms;
}

std::vector<AudioTrackInfo> MpegPsDemuxer::audioTracks() const {
    std::vector<AudioTrackInfo> tracks;
    if (audio_.present) {
        tracks.push_back(audio_);
    }
    return tracks;
}

bool MpegPsDemuxer::selectAudioTrack(size_t index) {
    return audio_.present && index == 0;
}

bool MpegPsDemuxer::readAudioSample(Sample& sample) {
    if (!audio_.present || next_audio_ >= audio_frames_.size()) {
        return false;
    }
    const AudioFrame& frame = audio_frames_[next_audio_];
    const size_t offset = static_cast<size_t>(frame.offset);
    const size_t size = static_cast<size_t>(frame.size);
    if (offset + size > audio_es_.size()) {
        return false;
    }
    sample.data.assign(audio_es_.begin() + static_cast<ptrdiff_t>(offset),
                       audio_es_.begin() + static_cast<ptrdiff_t>(offset + size));
    sample.pts = frame.pts_ms;
    sample.dts = frame.pts_ms;
    sample.keyframe = true;
    sample.track = TrackType::Audio;
    ++next_audio_;
    return true;
}

bool MpegPsDemuxer::seek(double seconds) {
    if (pictures_.empty()) {
        if (audio_frames_.empty()) {
            return false;
        }
        if (seconds < 0.0) {
            seconds = 0.0;
        }
        const int64_t target_ms = static_cast<int64_t>(seconds * 1000.0);
        size_t index = 0;
        for (size_t i = 0; i < audio_frames_.size(); ++i) {
            if (audio_frames_[i].pts_ms > target_ms) {
                break;
            }
            index = i;
        }
        next_audio_ = index;
        return true;
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
