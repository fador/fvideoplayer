#include "demux/MkvDemuxer.hpp"

#include "demux/HvcC.hpp"

#include <algorithm>
#include <cstring>

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
constexpr uint32_t kReferenceBlock = 0xFB;
constexpr uint32_t kCues = 0x1C53BB6B;
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
    if (size == 4) {
        float value = 0.0f;
        std::memcpy(&value, data, 4);
        return static_cast<double>(value);
    }
    if (size == 8) {
        double value = 0.0;
        std::memcpy(&value, data, 8);
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
    std::vector<uint8_t> codec_private;
    uint32_t width = 0;
    uint32_t height = 0;
    double sample_rate = 0.0;
    uint32_t channels = 0;
    uint64_t default_duration_ns = 0;
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
    return true;
}

uint32_t codecFourcc(const std::string& codec_id) {
    if (codec_id == "A_AAC") return 0x6D703461;      // 'mp4a'
    if (codec_id == "A_OPUS") return 0x4F707573;     // 'Opus'
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

// Reads an EBML variable-length integer from memory (track number, lacing).
bool readMemVint(const uint8_t* data, size_t size, size_t& pos, uint64_t& value, int* length_out) {
    if (pos >= size) {
        return false;
    }
    const uint8_t first = data[pos];
    if (first == 0) {
        return false;
    }
    int length = 1;
    uint8_t mask = 0x80;
    while (!(first & mask)) {
        mask >>= 1;
        ++length;
    }
    if (length > 8 || pos + static_cast<size_t>(length) > size) {
        return false;
    }
    value = first & (mask - 1);
    for (int i = 1; i < length; ++i) {
        value = (value << 8) | data[pos + static_cast<size_t>(i)];
    }
    pos += static_cast<size_t>(length);
    if (length_out) {
        *length_out = length;
    }
    return true;
}

bool readMemSignedVint(const uint8_t* data, size_t size, size_t& pos, int64_t& value) {
    uint64_t raw = 0;
    int length = 0;
    if (!readMemVint(data, size, pos, raw, &length)) {
        return false;
    }
    const uint64_t bias = (1ULL << (7 * length - 1)) - 1;
    value = static_cast<int64_t>(raw) - static_cast<int64_t>(bias);
    return true;
}

struct FrameRange {
    size_t offset = 0;
    size_t size = 0;
};

bool splitLacedFrames(const uint8_t* data, size_t size, size_t pos, uint8_t lacing,
                      std::vector<FrameRange>& frames) {
    frames.clear();
    const size_t remaining = size - pos;
    if (lacing == 0) {
        frames.push_back({pos, remaining});
        return true;
    }
    if (pos >= size) {
        return false;
    }
    const uint32_t frame_count = static_cast<uint32_t>(data[pos++]) + 1;
    if (frame_count == 1) {
        frames.push_back({pos, size - pos});
        return true;
    }

    if (lacing == 1) {  // Xiph
        std::vector<size_t> sizes;
        size_t total = 0;
        for (uint32_t i = 0; i + 1 < frame_count; ++i) {
            size_t frame_size = 0;
            for (;;) {
                if (pos >= size) {
                    return false;
                }
                const uint8_t byte = data[pos++];
                frame_size += byte;
                if (byte != 255) {
                    break;
                }
            }
            sizes.push_back(frame_size);
            total += frame_size;
        }
        if (size - pos < total) {
            return false;
        }
        for (size_t frame_size : sizes) {
            frames.push_back({pos, frame_size});
            pos += frame_size;
        }
        frames.push_back({pos, size - pos});
        return true;
    }
    if (lacing == 2) {  // fixed-size
        const size_t body = size - pos;
        if (frame_count == 0 || body % frame_count != 0) {
            return false;
        }
        const size_t frame_size = body / frame_count;
        for (uint32_t i = 0; i < frame_count; ++i) {
            frames.push_back({pos, frame_size});
            pos += frame_size;
        }
        return true;
    }
    // EBML lacing
    std::vector<size_t> sizes;
    size_t total = 0;
    int64_t previous = 0;
    for (uint32_t i = 0; i < frame_count; ++i) {
        if (i == 0) {
            uint64_t first_size = 0;
            if (!readMemVint(data, size, pos, first_size, nullptr)) {
                return false;
            }
            previous = static_cast<int64_t>(first_size);
        } else if (i + 1 < frame_count) {
            int64_t delta = 0;
            if (!readMemSignedVint(data, size, pos, delta)) {
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
    if (size - pos < total) {
        return false;
    }
    for (size_t frame_size : sizes) {
        frames.push_back({pos, frame_size});
        pos += frame_size;
    }
    frames.push_back({pos, size - pos});
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
    pos = segment_start;
    while (pos < segment_end) {
        if (!readElementHeader(file_, id, size, unknown)) {
            break;
        }
        const int64_t payload = tellFile(file_);
        if (id == kInfo && !unknown) {
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
        }
        if (unknown) {
            break;
        }
        pos = payload + static_cast<int64_t>(size);
        if (!seekFile(file_, pos)) {
            break;
        }
    }

    uint64_t video_track_number = 0;
    MkvTrack video_track;
    MkvTrack audio_track;
    bool have_video = false;
    bool have_audio = false;
    for (MkvTrack& track : tracks) {
        if (track.type == 1 && track.codec == "V_MPEGH/ISO/HEVC" && !have_video) {
            video_track = std::move(track);
            video_track_number = video_track.number;
            have_video = true;
        } else if (track.type == 2 && !have_audio) {
            audio_track = std::move(track);
            have_audio = true;
        }
    }
    if (!have_video) {
        error = "no HEVC video track found in Matroska file";
        return false;
    }

    // Pass 2: scan clusters and build the sample table.
    if (!seekFile(file_, segment_start)) {
        error = "failed to seek to Segment";
        return false;
    }

    auto addBlockSamples = [&](const uint8_t* block, size_t block_size, size_t block_offset,
                               int64_t payload_offset, int64_t cluster_timestamp,
                               int keyframe_override) {
        size_t cursor = 0;
        uint64_t track_number = 0;
        int track_length = 0;
        if (!readMemVint(block, block_size, cursor, track_number, &track_length) ||
            cursor + 3 > block_size) {
            return;
        }
        const int16_t relative = static_cast<int16_t>((block[cursor] << 8) | block[cursor + 1]);
        cursor += 2;
        const uint8_t flags = block[cursor++];
        if (track_number != video_track_number) {
            return;
        }
        const bool keyframe =
            keyframe_override >= 0 ? (keyframe_override != 0) : ((flags & 0x80) != 0);
        std::vector<FrameRange> frames;
        if (!splitLacedFrames(block, block_size, cursor, static_cast<uint8_t>((flags >> 1) & 3),
                              frames)) {
            return;
        }
        for (const FrameRange& frame : frames) {
            SampleInfo sample;
            sample.offset = static_cast<uint64_t>(payload_offset +
                                                  static_cast<int64_t>(block_offset + frame.offset));
            sample.size = static_cast<uint32_t>(frame.size);
            sample.pts = cluster_timestamp + relative;
            sample.keyframe = keyframe;
            samples_.push_back(sample);
        }
    };

    pos = segment_start;
    while (pos < segment_end) {
        if (!readElementHeader(file_, id, size, unknown)) {
            break;
        }
        const int64_t payload = tellFile(file_);
        if (id == kCluster) {
            const bool cluster_unknown = unknown;
            const int64_t cluster_end =
                cluster_unknown ? segment_end : payload + static_cast<int64_t>(size);
            int64_t cluster_timestamp = 0;
            while (tellFile(file_) < cluster_end) {
                const int64_t element_start = tellFile(file_);
                uint32_t child_id = 0;
                uint64_t child_size = 0;
                bool child_unknown = false;
                if (!readElementHeader(file_, child_id, child_size, child_unknown)) {
                    break;
                }
                if (cluster_unknown && isTopLevelId(child_id)) {
                    seekFile(file_, element_start);
                    break;
                }
                if (child_unknown) {
                    break;
                }
                const int64_t child_payload = tellFile(file_);
                if (child_id == kClusterTimestamp) {
                    uint8_t buffer[8] = {};
                    const size_t count = child_size < 8 ? static_cast<size_t>(child_size) : 8;
                    if (std::fread(buffer, 1, count, file_) == count) {
                        cluster_timestamp = static_cast<int64_t>(readUint(buffer, count));
                    }
                } else if (child_id == kSimpleBlock) {
                    std::vector<uint8_t> data;
                    if (readFileBytes(file_, child_size, data)) {
                        addBlockSamples(data.data(), data.size(), 0, child_payload,
                                        cluster_timestamp, -1);
                    }
                } else if (child_id == kBlockGroup) {
                    std::vector<uint8_t> data;
                    if (readFileBytes(file_, child_size, data)) {
                        size_t group_pos = 0;
                        Element element;
                        const uint8_t* block = nullptr;
                        size_t block_size = 0;
                        size_t block_offset = 0;
                        bool has_reference = false;
                        while (nextElement(data.data(), data.size(), group_pos, element)) {
                            if (element.id == kBlock) {
                                block = element.payload;
                                block_size = element.size;
                                block_offset = static_cast<size_t>(element.payload - data.data());
                            } else if (element.id == kReferenceBlock) {
                                has_reference = true;
                            }
                        }
                        if (block) {
                            addBlockSamples(block, block_size, block_offset, child_payload,
                                            cluster_timestamp, has_reference ? 0 : 1);
                        }
                    }
                }
                pos = child_payload + static_cast<int64_t>(child_size);
                if (!seekFile(file_, pos)) {
                    break;
                }
            }
            pos = tellFile(file_);
            continue;
        }
        if (unknown) {
            break;
        }
        pos = payload + static_cast<int64_t>(size);
        if (!seekFile(file_, pos)) {
            break;
        }
    }

    if (samples_.empty()) {
        error = "no video samples found in Matroska clusters";
        return false;
    }

    video_ = VideoTrackInfo{};
    video_.codec = Codec::Hevc;
    video_.timescale = 1000;  // milliseconds
    video_.format.annexb = false;
    video_.format.nal_length_size = 4;
    video_.width = video_track.width;
    video_.height = video_track.height;
    video_.hvcc = video_track.codec_private;
    if (!video_.hvcc.empty()) {
        HvccInfo hvcc;
        std::string hvcc_error;
        if (parseHvcc(video_.hvcc.data(), video_.hvcc.size(), hvcc, &hvcc_error)) {
            video_.format.nal_length_size = hvcc.length_size;
        }
    }

    // Convert timestamps to milliseconds when the scale is not 1 ms.
    if (timestamp_scale != 1000000) {
        for (SampleInfo& sample : samples_) {
            sample.pts = sample.pts * static_cast<int64_t>(timestamp_scale) / 1000000;
        }
    }

    for (size_t i = 0; i < samples_.size(); ++i) {
        if (samples_[i].keyframe) {
            keyframes_.emplace_back(samples_[i].pts, i);
        }
    }
    std::sort(keyframes_.begin(), keyframes_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    if (duration_ticks > 0.0) {
        duration_ = duration_ticks * static_cast<double>(timestamp_scale) / 1e9;
    } else {
        duration_ = static_cast<double>(samples_.back().pts - samples_.front().pts) / 1000.0;
    }
    if (video_track.default_duration_ns > 0) {
        video_.fps = 1e9 / static_cast<double>(video_track.default_duration_ns);
    } else if (duration_ > 0.0) {
        video_.fps = static_cast<double>(samples_.size()) / duration_;
    }

    if (have_audio) {
        audio_.present = true;
        audio_.codec_fourcc = codecFourcc(audio_track.codec);
        audio_.channels = audio_track.channels;
        audio_.sample_rate = static_cast<uint32_t>(audio_track.sample_rate);
        audio_.extradata = audio_track.codec_private;
    }

    next_sample_ = 0;
    return true;
}

bool MkvDemuxer::readSample(Sample& sample) {
    if (!file_ || next_sample_ >= samples_.size()) {
        return false;
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

bool MkvDemuxer::seek(double seconds) {
    if (samples_.empty()) {
        return false;
    }
    if (seconds < 0.0) {
        seconds = 0.0;
    }
    const int64_t target = static_cast<int64_t>(seconds * video_.timescale);

    size_t index = 0;
    const auto it = std::upper_bound(
        keyframes_.begin(), keyframes_.end(), target,
        [](int64_t value, const std::pair<int64_t, size_t>& entry) { return value < entry.first; });
    if (it != keyframes_.begin()) {
        index = std::prev(it)->second;
    } else if (!keyframes_.empty()) {
        index = keyframes_.front().second;
    }
    next_sample_ = index;
    return true;
}

}  // namespace player::demux
