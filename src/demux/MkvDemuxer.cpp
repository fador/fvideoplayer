#include "demux/MkvDemuxer.hpp"

#include "demux/Avcc.hpp"
#include "demux/HvcC.hpp"

#include <algorithm>
#include <cstring>
#include <unordered_map>

namespace player::demux {
namespace {

// EBML element IDs (including the length marker bits).
constexpr uint32_t kEbmlHeader = 0x1A45DFA3;
constexpr uint32_t kDocType = 0x4282;
constexpr uint32_t kSegment = 0x18538067;
constexpr uint32_t kSeekHead = 0x114D9B74;
constexpr uint32_t kInfo = 0x1549A966;
constexpr uint32_t kTimestampScale = 0x2AD7B1;
constexpr uint32_t kDuration = 0x4489;
constexpr uint32_t kTracks = 0x1654AE6B;
constexpr uint32_t kTrackEntry = 0xAE;
constexpr uint32_t kTrackNumber = 0xD7;
constexpr uint32_t kTrackType = 0x83;
constexpr uint32_t kCodecID = 0x86;
constexpr uint32_t kCodecPrivate = 0x63A2;
constexpr uint32_t kTrackName = 0x536E;
constexpr uint32_t kLanguage = 0x22B59C;
constexpr uint32_t kFlagDefault = 0x88;
constexpr uint32_t kDefaultDuration = 0x23E383;
constexpr uint32_t kVideo = 0xE0;
constexpr uint32_t kPixelWidth = 0xB0;
constexpr uint32_t kPixelHeight = 0xBA;
constexpr uint32_t kAudio = 0xE1;
constexpr uint32_t kSamplingFrequency = 0xB5;
constexpr uint32_t kChannels = 0x9F;
constexpr uint32_t kCluster = 0x1F43B675;
constexpr uint32_t kClusterTimestamp = 0xE7;
constexpr uint32_t kSimpleBlock = 0xA3;
constexpr uint32_t kBlockGroup = 0xA0;
constexpr uint32_t kBlock = 0xA1;
constexpr uint32_t kBlockDuration = 0x9B;
constexpr uint32_t kReferenceBlock = 0xFB;
constexpr uint32_t kCues = 0x1C53BB6B;
constexpr uint32_t kCuePoint = 0xBB;
constexpr uint32_t kCueTime = 0xB3;
constexpr uint32_t kCueTrackPositions = 0xB7;
constexpr uint32_t kCueTrack = 0xF7;
constexpr uint32_t kCueClusterPosition = 0xF1;
constexpr uint32_t kSeek = 0x4DBB;
constexpr uint32_t kSeekID = 0x53AB;
constexpr uint32_t kSeekPosition = 0x53AC;
constexpr uint32_t kVoid = 0xEC;
constexpr uint32_t kCrc32 = 0xBF;
constexpr uint32_t kAttachments = 0x1941A469;
constexpr uint32_t kChapters = 0x1043A770;
constexpr uint32_t kTags = 0x1254C367;

bool isTopLevelId(uint32_t id) {
    switch (id) {
        case kSeekHead:
        case kInfo:
        case kTracks:
        case kCluster:
        case kCues:
        case kVoid:
        case kCrc32:
        case kAttachments:
        case kChapters:
        case kTags:
            return true;
        default:
            return false;
    }
}

bool seekFile(FILE* file, int64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, offset, SEEK_SET) == 0;
#else
    return fseeko(file, static_cast<off_t>(offset), SEEK_SET) == 0;
#endif
}

int64_t tellFile(FILE* file) {
#ifdef _WIN32
    return _ftelli64(file);
#else
    return static_cast<int64_t>(ftello(file));
#endif
}

bool readId(FILE* file, uint32_t& id) {
    uint8_t first = 0;
    if (std::fread(&first, 1, 1, file) != 1 || first == 0) {
        return false;
    }
    int length = 1;
    uint8_t mask = 0x80;
    while (!(first & mask)) {
        mask >>= 1;
        ++length;
    }
    if (length > 4) {
        return false;
    }
    id = first;
    for (int i = 1; i < length; ++i) {
        uint8_t byte = 0;
        if (std::fread(&byte, 1, 1, file) != 1) {
            return false;
        }
        id = (id << 8) | byte;
    }
    return true;
}

bool readVint(FILE* file, uint64_t& value, bool& unknown) {
    uint8_t first = 0;
    if (std::fread(&first, 1, 1, file) != 1 || first == 0) {
        return false;
    }
    int length = 1;
    uint8_t mask = 0x80;
    while (!(first & mask)) {
        mask >>= 1;
        ++length;
    }
    if (length > 8) {
        return false;
    }
    value = first & (mask - 1);
    for (int i = 1; i < length; ++i) {
        uint8_t byte = 0;
        if (std::fread(&byte, 1, 1, file) != 1) {
            return false;
        }
        value = (value << 8) | byte;
    }
    const uint64_t all = (length == 8) ? ~0ULL : ((1ULL << (7 * length)) - 1);
    unknown = value == all;
    return true;
}

bool readElementHeader(FILE* file, uint32_t& id, uint64_t& size, bool& unknown) {
    return readId(file, id) && readVint(file, size, unknown);
}

bool readFileBytes(FILE* file, uint64_t size, std::vector<uint8_t>& out) {
    if (size > 64u * 1024u * 1024u) {
        return false;
    }
    out.resize(static_cast<size_t>(size));
    return out.empty() || std::fread(out.data(), 1, out.size(), file) == out.size();
}

// --- in-memory EBML helpers -------------------------------------------------

struct Element {
    uint32_t id = 0;
    const uint8_t* payload = nullptr;
    size_t size = 0;
};

bool nextElement(const uint8_t* data, size_t size, size_t& pos, Element& out) {
    if (pos >= size) {
        return false;
    }
    const uint8_t first = data[pos];
    if (first == 0) {
        return false;
    }
    int id_length = 1;
    uint8_t mask = 0x80;
    while (!(first & mask)) {
        mask >>= 1;
        ++id_length;
    }
    if (id_length > 4 || pos + static_cast<size_t>(id_length) > size) {
        return false;
    }
    uint32_t id = 0;
    for (int i = 0; i < id_length; ++i) {
        id = (id << 8) | data[pos + static_cast<size_t>(i)];
    }
    size_t cursor = pos + static_cast<size_t>(id_length);
    if (cursor >= size) {
        return false;
    }
    const uint8_t size_first = data[cursor];
    if (size_first == 0) {
        return false;
    }
    int size_length = 1;
    mask = 0x80;
    while (!(size_first & mask)) {
        mask >>= 1;
        ++size_length;
    }
    if (size_length > 8 || cursor + static_cast<size_t>(size_length) > size) {
        return false;
    }
    uint64_t element_size = size_first & (mask - 1);
    for (int i = 1; i < size_length; ++i) {
        element_size = (element_size << 8) | data[cursor + static_cast<size_t>(i)];
    }
    const uint64_t all = (size_length == 8) ? ~0ULL : ((1ULL << (7 * size_length)) - 1);
    if (element_size == all) {
        return false;  // unknown sizes are only supported while streaming
    }
    cursor += static_cast<size_t>(size_length);
    if (cursor + element_size > size) {
        return false;
    }
    out.id = id;
    out.payload = data + cursor;
    out.size = static_cast<size_t>(element_size);
    pos = cursor + static_cast<size_t>(element_size);
    return true;
}

uint64_t readUint(const uint8_t* data, size_t size) {
    uint64_t value = 0;
    const size_t count = size < 8 ? size : 8;
    for (size_t i = 0; i < count; ++i) {
        value = (value << 8) | data[i];
    }
    return value;
}

double readFloat(const uint8_t* data, size_t size) {
    // EBML floats are stored big-endian; build the value from the numeric bit
    // pattern so this works regardless of host endianness.
    if (size == 4) {
        const uint32_t bits = static_cast<uint32_t>(readUint(data, 4));
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return static_cast<double>(value);
    }
    if (size == 8) {
        const uint64_t bits = readUint(data, 8);
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }
    return 0.0;
}

std::string readString(const uint8_t* data, size_t size) {
    size_t length = size;
    while (length > 0 && data[length - 1] == 0) {
        --length;
    }
    return std::string(reinterpret_cast<const char*>(data), length);
}

struct MkvTrack {
    uint64_t number = 0;
    uint64_t type = 0;
    std::string codec;
    std::string name;
    std::string language;
    std::vector<uint8_t> codec_private;
    uint32_t width = 0;
    uint32_t height = 0;
    double sample_rate = 0.0;
    uint32_t channels = 0;
    uint64_t default_duration_ns = 0;
    bool default_track = true;
};

void parseVideoElement(const uint8_t* data, size_t size, MkvTrack& track) {
    size_t pos = 0;
    Element element;
    while (nextElement(data, size, pos, element)) {
        if (element.id == kPixelWidth) {
            track.width = static_cast<uint32_t>(readUint(element.payload, element.size));
        } else if (element.id == kPixelHeight) {
            track.height = static_cast<uint32_t>(readUint(element.payload, element.size));
        }
    }
}

void parseAudioElement(const uint8_t* data, size_t size, MkvTrack& track) {
    size_t pos = 0;
    Element element;
    while (nextElement(data, size, pos, element)) {
        if (element.id == kSamplingFrequency) {
            track.sample_rate = readFloat(element.payload, element.size);
        } else if (element.id == kChannels) {
            track.channels = static_cast<uint32_t>(readUint(element.payload, element.size));
        }
    }
}

bool parseTrackEntry(const uint8_t* data, size_t size, MkvTrack& track) {
    size_t pos = 0;
    Element element;
    while (nextElement(data, size, pos, element)) {
        switch (element.id) {
            case kTrackNumber:
                track.number = readUint(element.payload, element.size);
                break;
            case kTrackType:
                track.type = readUint(element.payload, element.size);
                break;
            case kCodecID:
                track.codec = readString(element.payload, element.size);
                break;
            case kCodecPrivate:
                track.codec_private.assign(element.payload, element.payload + element.size);
                break;
            case kTrackName:
                track.name = readString(element.payload, element.size);
                break;
            case kLanguage:
                track.language = readString(element.payload, element.size);
                break;
            case kFlagDefault:
                track.default_track = readUint(element.payload, element.size) != 0;
                break;
            case kDefaultDuration:
                track.default_duration_ns = readUint(element.payload, element.size);
                break;
            case kVideo:
                parseVideoElement(element.payload, element.size, track);
                break;
            case kAudio:
                parseAudioElement(element.payload, element.size, track);
                break;
            default:
                break;
        }
    }
    // Matroska defaults the track language to English when not present.
    if (track.language.empty()) {
        track.language = "eng";
    }
    return true;
}

std::string videoCodecName(const std::string& codec_id) {
    if (codec_id == "V_MPEGH/ISO/HEVC") return "HEVC";
    if (codec_id == "V_MPEG2") return "MPEG-2";
    if (codec_id == "V_MPEG1") return "MPEG-1";
    if (codec_id == "V_MPEG4/ISO/AVC") return "H.264";
    if (codec_id.rfind("V_MPEG4/ISO/", 0) == 0) return "MPEG-4 Visual";
    if (codec_id == "V_VP8") return "VP8";
    if (codec_id == "V_VP9") return "VP9";
    if (codec_id == "V_AV1") return "AV1";
    if (codec_id == "V_THEORA") return "Theora";
    if (codec_id == "V_MS/VFW/FOURCC") return "VFW";
    return codec_id;
}

uint32_t codecFourcc(const std::string& codec_id) {
    if (codec_id == "A_AAC") return 0x6D703461;      // 'mp4a'
    if (codec_id.rfind("A_OPUS", 0) == 0) return 0x4F707573;  // 'Opus', incl. A_OPUS/EXPERIMENTAL
    if (codec_id == "A_VORBIS") return 0x766F7262;   // 'vorb'
    if (codec_id == "A_FLAC") return 0x664C6143;     // 'fLaC'
    if (codec_id == "A_AC3") return 0x61632D33;      // 'ac-3'
    if (codec_id == "A_EAC3") return 0x65632D33;     // 'ec-3'
    if (codec_id == "A_DTS") return 0x64747363;      // 'dtsc'
    if (codec_id.rfind("A_PCM", 0) == 0) return 0x6C70636D;  // 'lpcm'
    uint32_t value = 0;
    for (size_t i = 0; i < 4 && i < codec_id.size(); ++i) {
        value = (value << 8) | static_cast<uint8_t>(codec_id[i]);
    }
    return value;
}

// Reads a signed EBML variable-length integer (EBML lacing deltas) from the
// file.
bool readSignedVint(FILE* file, int64_t& value) {
    uint8_t first = 0;
    if (std::fread(&first, 1, 1, file) != 1 || first == 0) {
        return false;
    }
    int length = 1;
    uint8_t mask = 0x80;
    while (!(first & mask)) {
        mask >>= 1;
        ++length;
    }
    if (length > 8) {
        return false;
    }
    uint64_t raw = first & (mask - 1);
    for (int i = 1; i < length; ++i) {
        uint8_t byte = 0;
        if (std::fread(&byte, 1, 1, file) != 1) {
            return false;
        }
        raw = (raw << 8) | byte;
    }
    const uint64_t bias = (1ULL << (7 * length - 1)) - 1;
    value = static_cast<int64_t>(raw) - static_cast<int64_t>(bias);
    return true;
}

struct FrameRange {
    size_t offset = 0;
    size_t size = 0;
};

struct BlockInfo {
    uint64_t track_number = 0;
    int16_t relative = 0;
    uint8_t flags = 0;
    std::vector<FrameRange> frames;
};

// Reads a block header and its lacing from the file, which must be positioned
// at the block payload. Only the header bytes are read; frame payloads are
// left untouched so opening a file never reads the whole file.
bool readBlockInfo(FILE* file, uint64_t block_size, BlockInfo& out) {
    const int64_t payload_start = tellFile(file);
    bool unknown = false;
    if (!readVint(file, out.track_number, unknown)) {
        return false;
    }
    uint8_t fixed[3] = {};
    if (std::fread(fixed, 1, 3, file) != 3) {
        return false;
    }
    out.relative = static_cast<int16_t>((fixed[0] << 8) | fixed[1]);
    out.flags = fixed[2];
    const uint8_t lacing = static_cast<uint8_t>((out.flags >> 1) & 3);
    out.frames.clear();
    size_t pos = static_cast<size_t>(tellFile(file) - payload_start);
    if (lacing == 0) {
        if (block_size < pos) {
            return false;
        }
        out.frames.push_back({pos, static_cast<size_t>(block_size) - pos});
        return true;
    }
    uint8_t count_byte = 0;
    if (std::fread(&count_byte, 1, 1, file) != 1) {
        return false;
    }
    ++pos;
    const uint32_t frame_count = static_cast<uint32_t>(count_byte) + 1;
    if (frame_count == 1) {
        if (block_size < pos) {
            return false;
        }
        out.frames.push_back({pos, static_cast<size_t>(block_size) - pos});
        return true;
    }
    std::vector<size_t> sizes;
    size_t total = 0;
    if (lacing == 1) {  // Xiph
        for (uint32_t i = 0; i + 1 < frame_count; ++i) {
            size_t frame_size = 0;
            for (;;) {
                uint8_t byte = 0;
                if (std::fread(&byte, 1, 1, file) != 1) {
                    return false;
                }
                ++pos;
                frame_size += byte;
                if (byte != 255) {
                    break;
                }
            }
            sizes.push_back(frame_size);
            total += frame_size;
        }
    } else if (lacing == 2) {  // fixed-size
        if (block_size < pos) {
            return false;
        }
        const size_t body = static_cast<size_t>(block_size) - pos;
        if (frame_count == 0 || body % frame_count != 0) {
            return false;
        }
        const size_t frame_size = body / frame_count;
        for (uint32_t i = 0; i < frame_count; ++i) {
            out.frames.push_back({pos, frame_size});
            pos += frame_size;
        }
        return true;
    } else {  // EBML lacing
        int64_t previous = 0;
        for (uint32_t i = 0; i < frame_count; ++i) {
            if (i == 0) {
                uint64_t first_size = 0;
                if (!readVint(file, first_size, unknown)) {
                    return false;
                }
                previous = static_cast<int64_t>(first_size);
            } else if (i + 1 < frame_count) {
                int64_t delta = 0;
                if (!readSignedVint(file, delta)) {
                    return false;
                }
                previous += delta;
            } else {
                break;  // last frame takes the remainder
            }
            if (previous < 0) {
                return false;
            }
            sizes.push_back(static_cast<size_t>(previous));
            total += static_cast<size_t>(previous);
        }
        pos = static_cast<size_t>(tellFile(file) - payload_start);
    }
    if (block_size < pos + total) {
        return false;
    }
    for (size_t frame_size : sizes) {
        out.frames.push_back({pos, frame_size});
        pos += frame_size;
    }
    out.frames.push_back({pos, static_cast<size_t>(block_size) - pos});
    return true;
}

}  // namespace

MkvDemuxer::MkvDemuxer() = default;

MkvDemuxer::~MkvDemuxer() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool MkvDemuxer::open(const std::filesystem::path& path, std::string& error) {
    path_ = path;
    file_ = std::fopen(path.string().c_str(), "rb");
    if (!file_) {
        error = "failed to open " + path.string();
        return false;
    }
    if (std::fseek(file_, 0, SEEK_END) != 0) {
        error = "failed to determine file size";
        return false;
    }
    file_size_ = tellFile(file_);

    if (!seekFile(file_, 0)) {
        error = "failed to seek input";
        return false;
    }
    uint32_t id = 0;
    uint64_t size = 0;
    bool unknown = false;
    if (!readElementHeader(file_, id, size, unknown) || id != kEbmlHeader || unknown) {
        error = "not an EBML/Matroska file";
        return false;
    }
    std::vector<uint8_t> header;
    if (!readFileBytes(file_, size, header)) {
        error = "failed to read EBML header";
        return false;
    }
    std::string doc_type;
    {
        size_t pos = 0;
        Element element;
        while (nextElement(header.data(), header.size(), pos, element)) {
            if (element.id == kDocType) {
                doc_type = readString(element.payload, element.size);
            }
        }
    }
    if (doc_type != "matroska" && doc_type != "webm") {
        error = "unsupported EBML DocType: " + doc_type;
        return false;
    }

    // Locate the Segment element.
    int64_t segment_start = -1;
    uint64_t segment_size = 0;
    bool segment_unknown = false;
    int64_t pos = tellFile(file_);
    while (pos < file_size_) {
        const int64_t before = tellFile(file_);
        if (!readElementHeader(file_, id, size, unknown)) {
            break;
        }
        if (id == kSegment) {
            segment_start = tellFile(file_);
            segment_size = size;
            segment_unknown = unknown;
            break;
        }
        if (unknown) {
            break;
        }
        pos = before + (tellFile(file_) - before) + static_cast<int64_t>(size);
        if (!seekFile(file_, pos)) {
            break;
        }
    }
    if (segment_start < 0) {
        error = "Matroska Segment element not found";
        return false;
    }
    const int64_t segment_end =
        segment_unknown ? file_size_ : segment_start + static_cast<int64_t>(segment_size);

    // Pass 1: read Info and Tracks.
    uint64_t timestamp_scale = 1000000;
    double duration_ticks = 0.0;
    std::vector<MkvTrack> tracks;
    if (!seekFile(file_, segment_start)) {
        error = "failed to seek to Segment";
        return false;
    }
    bool have_info = false;
    bool have_tracks = false;
    int64_t cues_position = -1;
    std::unordered_map<uint32_t, int64_t> seek_positions;
    pos = segment_start;
    while (pos < segment_end) {
        if (!readElementHeader(file_, id, size, unknown)) {
            break;
        }
        const int64_t payload = tellFile(file_);
        if (id == kInfo && !unknown) {
            have_info = true;
            std::vector<uint8_t> data;
            if (readFileBytes(file_, size, data)) {
                size_t cursor = 0;
                Element element;
                while (nextElement(data.data(), data.size(), cursor, element)) {
                    if (element.id == kTimestampScale) {
                        timestamp_scale = readUint(element.payload, element.size);
                    } else if (element.id == kDuration) {
                        duration_ticks = readFloat(element.payload, element.size);
                    }
                }
            }
        } else if (id == kTracks && !unknown) {
            have_tracks = true;
            std::vector<uint8_t> data;
            if (readFileBytes(file_, size, data)) {
                size_t cursor = 0;
                Element element;
                while (nextElement(data.data(), data.size(), cursor, element)) {
                    if (element.id == kTrackEntry) {
                        MkvTrack track;
                        parseTrackEntry(element.payload, element.size, track);
                        tracks.push_back(std::move(track));
                    }
                }
            }
        } else if (id == kSeekHead && !unknown) {
            std::vector<uint8_t> data;
            if (readFileBytes(file_, size, data)) {
                size_t cursor = 0;
                Element element;
                while (nextElement(data.data(), data.size(), cursor, element)) {
                    if (element.id != kSeek) {
                        continue;
                    }
                    size_t seek_cursor = 0;
                    Element child;
                    uint32_t target = 0;
                    int64_t target_position = -1;
                    while (nextElement(element.payload, element.size, seek_cursor, child)) {
                        if (child.id == kSeekID) {
                            target = static_cast<uint32_t>(readUint(child.payload, child.size));
                        } else if (child.id == kSeekPosition) {
                            target_position = static_cast<int64_t>(readUint(child.payload, child.size));
                        }
                    }
                    if (target != 0 && target_position >= 0) {
                        seek_positions[target] = target_position;
                    }
                }
            }
        } else if (id == kCues && !unknown) {
            cues_position = payload;
        }
        if (unknown) {
            break;
        }
        if (have_info && have_tracks) {
            // Info and Tracks always come before the clusters. Everything
            // else is either reached through the SeekHead or indexed lazily
            // while reading, so the open does not scan the whole file.
            break;
        }
        pos = payload + static_cast<int64_t>(size);
        if (!seekFile(file_, pos)) {
            break;
        }
    }

    uint64_t video_track_number = 0;
    MkvTrack video_track;
    bool have_video = false;
    Codec video_codec = Codec::Unknown;

    track_map_.clear();
    audio_tracks_.clear();
    audio_samples_.clear();
    subtitle_tracks_.clear();

    for (MkvTrack& track : tracks) {
        if (track.type == 1 && !have_video) {
            // Matroska codec ids for the video formats the player can decode.
            if (track.codec == "V_MPEGH/ISO/HEVC") {
                video_codec = Codec::Hevc;
            } else if (track.codec == "V_MPEG4/ISO/AVC") {
                video_codec = Codec::H264;
            } else if (track.codec == "V_MPEG2") {
                video_codec = Codec::Mpeg2;
            } else if (track.codec == "V_MPEG1") {
                video_codec = Codec::Mpeg1;
            }
            video_track = std::move(track);
            video_track_number = video_track.number;
            have_video = true;
            // A video codec without a decoder keeps its track description but
            // no sample table, so its blocks are skipped and only the audio
            // plays. The same goes for a track the player cannot decode.
            if (video_codec != Codec::Unknown) {
                track_map_[video_track_number] = {0, 0};
            }
        } else if (track.type == 2) {
            AudioTrackInfo info;
            info.index = audio_tracks_.size();
            info.name = track.name;
            info.language = track.language;
            info.codec = track.codec;
            info.present = true;
            info.default_track = track.default_track;
            info.codec_fourcc = codecFourcc(track.codec);
            info.channels = track.channels;
            info.sample_rate = static_cast<uint32_t>(track.sample_rate);
            info.extradata = track.codec_private;
            track_map_[track.number] = {1, audio_tracks_.size()};
            audio_tracks_.push_back(std::move(info));
            audio_samples_.emplace_back();
        } else if (track.type == 17) {
            SubtitleTrackInfo info;
            info.index = subtitle_tracks_.size();
            info.name = track.name;
            info.language = track.language;
            info.codec = track.codec;
            info.codec_private = track.codec_private;
            info.bitmap = track.codec == "S_HDMV/PGS" || track.codec == "S_VOBSUB" ||
                          track.codec == "S_DVBSUB";
            track_map_[track.number] = {2, subtitle_tracks_.size()};
            subtitle_tracks_.push_back(std::move(info));
        }
    }
    if (!have_video && audio_tracks_.empty()) {
        error = "no playable audio or video track found in Matroska file";
        return false;
    }
    if (!audio_tracks_.empty()) {
        selected_audio_ = 0;
        audio_ = audio_tracks_[0];
    }

    segment_start_ = segment_start;
    segment_end_ = segment_end;
    timestamp_scale_ = timestamp_scale;

    // Resolve the Cues element: directly if it preceded the clusters, or
    // through the SeekHead otherwise. Cues hold the keyframe positions used
    // for seeking without indexing the whole file.
    if (cues_position < 0) {
        const auto cue_seek = seek_positions.find(kCues);
        if (cue_seek != seek_positions.end()) {
            cues_position = segment_start + cue_seek->second;
        }
    }
    if (cues_position >= 0 && seekFile(file_, cues_position)) {
        uint32_t cue_id = 0;
        uint64_t cue_size = 0;
        bool cue_unknown = false;
        if (readElementHeader(file_, cue_id, cue_size, cue_unknown) && cue_id == kCues &&
            !cue_unknown) {
            std::vector<uint8_t> data;
            if (readFileBytes(file_, cue_size, data)) {
                size_t cursor = 0;
                Element cue_point;
                while (nextElement(data.data(), data.size(), cursor, cue_point)) {
                    if (cue_point.id != kCuePoint) {
                        continue;
                    }
                    size_t point_cursor = 0;
                    Element child;
                    int64_t time_ticks = -1;
                    int64_t cluster_offset = -1;
                    while (nextElement(cue_point.payload, cue_point.size, point_cursor, child)) {
                        if (child.id == kCueTime) {
                            time_ticks = static_cast<int64_t>(readUint(child.payload, child.size));
                        } else if (child.id == kCueTrackPositions) {
                            size_t positions_cursor = 0;
                            Element position_child;
                            uint64_t track_number = 0;
                            int64_t position = -1;
                            while (nextElement(child.payload, child.size, positions_cursor,
                                               position_child)) {
                                if (position_child.id == kCueTrack) {
                                    track_number =
                                        readUint(position_child.payload, position_child.size);
                                } else if (position_child.id == kCueClusterPosition) {
                                    position = static_cast<int64_t>(
                                        readUint(position_child.payload, position_child.size));
                                }
                            }
                            if (position >= 0 &&
                                (video_track_number == 0 || track_number == video_track_number)) {
                                cluster_offset = position;
                            }
                        }
                    }
                    if (time_ticks >= 0 && cluster_offset >= 0) {
                        Cue cue;
                        cue.time_ms =
                            time_ticks * static_cast<int64_t>(timestamp_scale) / 1000000;
                        cue.cluster_offset = cluster_offset;
                        cues_.push_back(cue);
                    }
                }
                std::sort(cues_.begin(), cues_.end(),
                          [](const Cue& a, const Cue& b) { return a.time_ms < b.time_ms; });
            }
        }
    }

    // Without cues the sample table has to be built up front; with them the
    // clusters are parsed while reading and seeking jumps by cue.
    full_index_ = cues_.empty();
    index_complete_ = false;
    index_pos_ = segment_start;
    if (full_index_) {
        while (parseNextCluster()) {
        }
        index_complete_ = true;
        for (size_t i = 0; i < samples_.size(); ++i) {
            if (samples_[i].keyframe) {
                keyframes_.emplace_back(samples_[i].pts, i);
            }
        }
        std::sort(keyframes_.begin(), keyframes_.end(),
                  [](const auto& a, const auto& b) { return a.first < b.first; });
    } else {
        parseNextCluster();
    }

    if (samples_.empty() && audio_tracks_.empty()) {
        error = "no video samples found in Matroska clusters";
        return false;
    }

    video_ = VideoTrackInfo{};
    video_.codec = video_codec;
    video_.timescale = 1000;  // milliseconds
    video_.format.annexb = false;
    video_.format.nal_length_size = 4;
    if (have_video) {
        video_.codec_name = videoCodecName(video_track.codec);
        video_.width = video_track.width;
        video_.height = video_track.height;
        if (video_codec == Codec::H264) {
            video_.avcc = video_track.codec_private;
        } else {
            video_.hvcc = video_track.codec_private;
        }
    }
    if (!video_.hvcc.empty()) {
        HvccInfo hvcc;
        std::string hvcc_error;
        if (parseHvcc(video_.hvcc.data(), video_.hvcc.size(), hvcc, &hvcc_error)) {
            video_.format.nal_length_size = hvcc.length_size;
        }
    }
    if (video_codec == Codec::H264 && !video_.avcc.empty()) {
        AvccInfo avcc;
        if (parseAvcc(video_.avcc.data(), video_.avcc.size(), avcc, nullptr)) {
            video_.format.nal_length_size = avcc.length_size;
        }
    }

    if (duration_ticks > 0.0) {
        duration_ = duration_ticks * static_cast<double>(timestamp_scale) / 1e9;
    } else if (full_index_ && !samples_.empty()) {
        duration_ = static_cast<double>(samples_.back().pts - samples_.front().pts) / 1000.0;
    } else if (!cues_.empty()) {
        duration_ = static_cast<double>(cues_.back().time_ms) / 1000.0;
    } else {
        duration_ = -1.0;
    }
    if (video_track.default_duration_ns > 0) {
        video_.fps = 1e9 / static_cast<double>(video_track.default_duration_ns);
    } else if (full_index_ && duration_ > 0.0) {
        video_.fps = static_cast<double>(samples_.size()) / duration_;
    } else if (samples_.size() >= 2) {
        // No DefaultDuration: estimate from the first cluster's timestamps.
        std::vector<int64_t> pts;
        pts.reserve(samples_.size());
        for (const SampleInfo& info : samples_) {
            pts.push_back(info.pts);
        }
        std::sort(pts.begin(), pts.end());
        int64_t best = 0;
        for (size_t i = 1; i < pts.size(); ++i) {
            const int64_t step = pts[i] - pts[i - 1];
            if (step > 0 && (best == 0 || step < best)) {
                best = step;
            }
        }
        if (best > 0) {
            video_.fps = 1000.0 / static_cast<double>(best);
        }
    }

    next_sample_ = 0;
    for (TrackSamples& track : audio_samples_) {
        track.next = 0;
    }
    return true;
}

bool MkvDemuxer::parseNextCluster() {
    while (!index_complete_) {
        if (index_pos_ < segment_start_ || index_pos_ >= segment_end_) {
            index_complete_ = true;
            break;
        }
        if (!seekFile(file_, index_pos_)) {
            index_complete_ = true;
            break;
        }
        uint32_t id = 0;
        uint64_t size = 0;
        bool unknown = false;
        if (!readElementHeader(file_, id, size, unknown)) {
            index_complete_ = true;
            break;
        }
        const int64_t payload = tellFile(file_);
        if (id == kCluster) {
            const int64_t cluster_end =
                unknown ? segment_end_ : payload + static_cast<int64_t>(size);
            index_pos_ = parseCluster(file_, payload, cluster_end, unknown, &samples_,
                                      &audio_samples_, nullptr);
            return true;
        }
        if (unknown) {
            index_complete_ = true;
            break;
        }
        index_pos_ = payload + static_cast<int64_t>(size);
    }
    return false;
}

int64_t MkvDemuxer::parseCluster(FILE* file, int64_t payload, int64_t cluster_end, bool unknown,
                                 std::vector<SampleInfo>* video,
                                 std::vector<TrackSamples>* audio,
                                 std::vector<TrackSamples>* subtitle) {
    int64_t cluster_timestamp = 0;
    int64_t pos = payload;
    while (pos < cluster_end) {
        if (!seekFile(file, pos)) {
            break;
        }
        const int64_t element_start = pos;
        uint32_t child_id = 0;
        uint64_t child_size = 0;
        bool child_unknown = false;
        if (!readElementHeader(file, child_id, child_size, child_unknown)) {
            break;
        }
        if (unknown && isTopLevelId(child_id)) {
            return element_start;
        }
        if (child_unknown) {
            break;
        }
        const int64_t child_payload = tellFile(file);
        if (child_id == kClusterTimestamp) {
            uint8_t buffer[8] = {};
            const size_t count = child_size < 8 ? static_cast<size_t>(child_size) : 8;
            if (std::fread(buffer, 1, count, file) == count) {
                cluster_timestamp = static_cast<int64_t>(readUint(buffer, count));
            }
        } else if (child_id == kSimpleBlock) {
            appendBlockSamples(file, child_payload, child_size, cluster_timestamp, -1, 0, video,
                               audio, subtitle);
        } else if (child_id == kBlockGroup) {
            const int64_t group_end = child_payload + static_cast<int64_t>(child_size);
            int64_t block_payload = -1;
            uint64_t block_size = 0;
            bool has_reference = false;
            int64_t block_duration = 0;
            int64_t group_pos = child_payload;
            while (group_pos < group_end) {
                if (!seekFile(file, group_pos)) {
                    break;
                }
                uint32_t group_id = 0;
                uint64_t group_size = 0;
                bool group_unknown = false;
                if (!readElementHeader(file, group_id, group_size, group_unknown) ||
                    group_unknown) {
                    break;
                }
                const int64_t group_payload = tellFile(file);
                if (group_id == kBlock) {
                    block_payload = group_payload;
                    block_size = group_size;
                } else if (group_id == kBlockDuration) {
                    uint8_t buffer[8] = {};
                    const size_t count = group_size < 8 ? static_cast<size_t>(group_size) : 8;
                    if (std::fread(buffer, 1, count, file) == count) {
                        block_duration = static_cast<int64_t>(readUint(buffer, count));
                    }
                } else if (group_id == kReferenceBlock) {
                    has_reference = true;
                }
                group_pos = group_payload + static_cast<int64_t>(group_size);
            }
            if (block_payload >= 0 && seekFile(file, block_payload)) {
                appendBlockSamples(file, block_payload, block_size, cluster_timestamp,
                                   has_reference ? 0 : 1, block_duration, video, audio, subtitle);
            }
        }
        pos = child_payload + static_cast<int64_t>(child_size);
    }
    return cluster_end;
}

void MkvDemuxer::appendBlockSamples(FILE* file, int64_t block_payload, uint64_t block_size,
                                    int64_t cluster_timestamp, int keyframe_override,
                                    int64_t block_duration, std::vector<SampleInfo>* video,
                                    std::vector<TrackSamples>* audio,
                                    std::vector<TrackSamples>* subtitle) {
    BlockInfo info;
    if (!readBlockInfo(file, block_size, info)) {
        return;
    }
    const auto ref = track_map_.find(info.track_number);
    if (ref == track_map_.end()) {
        return;
    }
    const bool keyframe =
        keyframe_override >= 0 ? (keyframe_override != 0) : ((info.flags & 0x80) != 0);
    for (const FrameRange& frame : info.frames) {
        SampleInfo sample;
        sample.offset = static_cast<uint64_t>(block_payload + static_cast<int64_t>(frame.offset));
        sample.size = static_cast<uint32_t>(frame.size);
        sample.pts = toMs(cluster_timestamp + info.relative);
        sample.keyframe = keyframe;
        sample.duration = toMs(block_duration);
        if (ref->second.kind == 0) {
            if (video != nullptr) {
                video->push_back(sample);
            }
        } else if (ref->second.kind == 1) {
            if (audio != nullptr && ref->second.index < audio->size()) {
                (*audio)[ref->second.index].samples.push_back(sample);
            }
        } else if (subtitle != nullptr && ref->second.index < subtitle->size()) {
            (*subtitle)[ref->second.index].samples.push_back(sample);
        }
    }
}

bool MkvDemuxer::readSample(Sample& sample) {
    while (next_sample_ >= samples_.size()) {
        if (index_complete_ || !parseNextCluster()) {
            return false;
        }
    }
    const SampleInfo& info = samples_[next_sample_];
    sample.data.resize(info.size);
    if (info.size > 0) {
        if (!seekFile(file_, static_cast<int64_t>(info.offset))) {
            return false;
        }
        if (std::fread(sample.data.data(), 1, info.size, file_) != info.size) {
            return false;
        }
    }
    sample.pts = info.pts;
    sample.dts = info.pts;
    sample.keyframe = info.keyframe;
    sample.track = TrackType::Video;
    ++next_sample_;
    return true;
}

bool MkvDemuxer::selectAudioTrack(size_t index) {
    if (index >= audio_tracks_.size() || index >= audio_samples_.size()) {
        return false;
    }
    selected_audio_ = index;
    audio_samples_[index].next = 0;
    audio_ = audio_tracks_[index];
    return true;
}

bool MkvDemuxer::readAudioSample(Sample& sample) {
    if (audio_samples_.empty() || selected_audio_ >= audio_samples_.size()) {
        return false;
    }
    TrackSamples& track = audio_samples_[selected_audio_];
    while (track.next >= track.samples.size()) {
        if (index_complete_ || !parseNextCluster()) {
            return false;
        }
    }
    const SampleInfo& info = track.samples[track.next];
    sample.data.resize(info.size);
    if (info.size > 0) {
        if (!seekFile(file_, static_cast<int64_t>(info.offset))) {
            return false;
        }
        if (std::fread(sample.data.data(), 1, info.size, file_) != info.size) {
            return false;
        }
    }
    sample.pts = info.pts;
    sample.dts = info.pts;
    sample.keyframe = true;
    sample.track = TrackType::Audio;
    ++track.next;
    return true;
}

bool MkvDemuxer::readSubtitleTrack(size_t index, std::vector<subtitle::SubtitleBlock>& blocks,
                                   std::string& error) {
    blocks.clear();
    if (index >= subtitle_tracks_.size()) {
        error = "subtitle track index out of range";
        return false;
    }

    // Use a private file handle: this may be called from the UI thread while
    // the pipeline reads video samples through file_. The subtitle samples are
    // collected on demand so opening a file does not index them.
    FILE* file = std::fopen(path_.string().c_str(), "rb");
    if (!file) {
        error = "failed to reopen " + path_.string();
        return false;
    }
    std::vector<TrackSamples> scan(subtitle_tracks_.size());
    int64_t pos = segment_start_;
    while (pos < segment_end_) {
        if (!seekFile(file, pos)) {
            break;
        }
        uint32_t id = 0;
        uint64_t size = 0;
        bool unknown = false;
        if (!readElementHeader(file, id, size, unknown)) {
            break;
        }
        const int64_t payload = tellFile(file);
        if (id == kCluster) {
            const int64_t cluster_end =
                unknown ? segment_end_ : payload + static_cast<int64_t>(size);
            pos = parseCluster(file, payload, cluster_end, unknown, nullptr, nullptr, &scan);
            continue;
        }
        if (unknown) {
            break;
        }
        pos = payload + static_cast<int64_t>(size);
    }

    const TrackSamples& track = scan[index];
    if (track.samples.empty()) {
        std::fclose(file);
        error = "subtitle track has no blocks";
        return false;
    }
    bool ok = true;
    for (const SampleInfo& info : track.samples) {
        subtitle::SubtitleBlock block;
        block.data.resize(info.size);
        if (info.size > 0) {
            if (!seekFile(file, static_cast<int64_t>(info.offset)) ||
                std::fread(block.data.data(), 1, info.size, file) != info.size) {
                ok = false;
                break;
            }
        }
        block.pts_ms = info.pts;
        block.duration_ms = info.duration;
        blocks.push_back(std::move(block));
    }
    std::fclose(file);
    if (!ok) {
        error = "failed to read subtitle block data";
        blocks.clear();
        return false;
    }
    return true;
}

bool MkvDemuxer::seek(double seconds) {
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    const int64_t target = static_cast<int64_t>(seconds * video_.timescale);

    if (!full_index_) {
        // Jump to the keyframe cue at or before the target and refill the
        // queues from its cluster; the following clusters are parsed on
        // demand. This makes seeking cheap even on files that were opened
        // without scanning them.
        int64_t start = segment_start_;
        if (!cues_.empty()) {
            auto it = std::upper_bound(
                cues_.begin(), cues_.end(), target,
                [](int64_t value, const Cue& cue) { return value < cue.time_ms; });
            if (it != cues_.begin()) {
                --it;
            }
            start = segment_start_ + it->cluster_offset;
        }
        samples_.clear();
        next_sample_ = 0;
        for (TrackSamples& track : audio_samples_) {
            track.samples.clear();
            track.next = 0;
        }
        index_pos_ = start;
        index_complete_ = false;
        if (!parseNextCluster()) {
            return false;
        }
        // Keep one audio packet before the target as a lead-in, like the
        // fully indexed path does.
        for (TrackSamples& track : audio_samples_) {
            size_t next = 0;
            while (next + 1 < track.samples.size() && track.samples[next + 1].pts <= target) {
                ++next;
            }
            track.next = next;
        }
        return true;
    }

    if (samples_.empty() && audio_samples_.empty()) {
        return false;
    }

    if (!samples_.empty()) {
        size_t index = 0;
        const auto it = std::upper_bound(
            keyframes_.begin(), keyframes_.end(), target,
            [](int64_t value, const std::pair<int64_t, size_t>& entry) {
                return value < entry.first;
            });
        if (it != keyframes_.begin()) {
            index = std::prev(it)->second;
        } else if (!keyframes_.empty()) {
            index = keyframes_.front().second;
        }
        next_sample_ = index;
    }

    // Every track keeps its own cursor, so the audio ones have to move as well.
    // A cursor left ahead of the new playhead hands the player packets from the
    // future: the audio buffer fills with chunks that are not due yet and the
    // sink never plays them, so playback stays silent until the track is
    // selected again. Timestamps are milliseconds for every track.
    for (TrackSamples& track : audio_samples_) {
        const auto first_after = std::lower_bound(
            track.samples.begin(), track.samples.end(), target,
            [](const SampleInfo& info, int64_t value) { return info.pts < value; });
        // Step back to the packet that covers the target so the decoder gets a
        // short lead instead of starting late.
        track.next = first_after == track.samples.begin()
                         ? 0
                         : static_cast<size_t>(std::prev(first_after) -
                                               track.samples.begin());
    }
    return true;
}

}  // namespace player::demux
