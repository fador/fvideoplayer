#include "demux/AviDemuxer.hpp"

#include "demux/HvcC.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace player::demux {
namespace {

constexpr uint32_t fourcc(char a, char b, char c, char d) {
    return (static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24) |
           (static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16) |
           (static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8) |
           static_cast<uint32_t>(static_cast<uint8_t>(d));
}

uint32_t readU32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint16_t readU16(const uint8_t* data) {
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
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

bool readAt(FILE* file, int64_t offset, void* out, size_t size) {
    return seekFile(file, offset) && std::fread(out, 1, size, file) == size;
}

struct ChunkHeader {
    uint32_t id = 0;
    uint32_t size = 0;
    int64_t payload = 0;
};

bool readChunkHeader(FILE* file, ChunkHeader& out) {
    uint8_t header[8] = {};
    if (std::fread(header, 1, sizeof(header), file) != sizeof(header)) {
        return false;
    }
    out.id = readU32(header);
    out.size = readU32(header + 4);
    out.payload = tellFile(file);
    return true;
}

bool isHevcFourcc(uint32_t value) {
    return value == fourcc('H', 'E', 'V', 'C') || value == fourcc('H', '2', '6', '5') ||
           value == fourcc('h', 'v', 'c', '1') || value == fourcc('h', 'e', 'v', '1') ||
           value == fourcc('x', '2', '6', '5');
}

struct AviStream {
    uint32_t fcc_type = 0;
    uint32_t fcc_handler = 0;
    uint32_t scale = 0;
    uint32_t rate = 0;
    uint32_t length = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t compression = 0;
    std::vector<uint8_t> extradata;
    uint16_t audio_format_tag = 0;
    uint16_t audio_channels = 0;
    uint32_t audio_sample_rate = 0;
};

bool parseStrh(const uint8_t* data, size_t size, AviStream& stream) {
    if (size < 40) {
        return false;
    }
    stream.fcc_type = readU32(data);
    stream.fcc_handler = readU32(data + 4);
    stream.scale = readU32(data + 20);
    stream.rate = readU32(data + 24);
    stream.length = readU32(data + 32);
    return true;
}

bool parseStrf(const uint8_t* data, size_t size, AviStream& stream) {
    if (size < 16) {
        return false;
    }
    if (stream.fcc_type == fourcc('v', 'i', 'd', 's')) {
        if (size < 40) {
            return false;
        }
        stream.width = readU32(data + 4);
        const int32_t height = static_cast<int32_t>(readU32(data + 8));
        stream.height = height < 0 ? static_cast<uint32_t>(-height) : static_cast<uint32_t>(height);
        stream.compression = readU32(data + 16);
        if (size > 40) {
            stream.extradata.assign(data + 40, data + size);
        }
    } else if (stream.fcc_type == fourcc('a', 'u', 'd', 's')) {
        stream.audio_format_tag = readU16(data);
        stream.audio_channels = readU16(data + 2);
        stream.audio_sample_rate = readU32(data + 4);
    }
    return true;
}

bool parseHdrl(FILE* file, int64_t start, int64_t size, std::vector<AviStream>& streams) {
    const int64_t end = start + size;
    int64_t pos = start;
    while (pos + 8 <= end) {
        if (!seekFile(file, pos)) {
            return false;
        }
        ChunkHeader chunk;
        if (!readChunkHeader(file, chunk)) {
            return false;
        }
        if (chunk.id == fourcc('L', 'I', 'S', 'T') && chunk.size >= 4) {
            uint8_t type_bytes[4] = {};
            if (!readAt(file, chunk.payload, type_bytes, 4)) {
                return false;
            }
            if (readU32(type_bytes) == fourcc('s', 't', 'r', 'l')) {
                AviStream stream;
                const int64_t strl_end = chunk.payload + static_cast<int64_t>(chunk.size);
                int64_t strl_pos = chunk.payload + 4;
                bool have_strh = false;
                while (strl_pos + 8 <= strl_end) {
                    if (!seekFile(file, strl_pos)) {
                        return false;
                    }
                    ChunkHeader child;
                    if (!readChunkHeader(file, child)) {
                        return false;
                    }
                    if (child.id == fourcc('s', 't', 'r', 'h') && child.size >= 40) {
                        uint8_t buffer[56] = {};
                        const size_t count = child.size < sizeof(buffer) ? child.size : sizeof(buffer);
                        if (!readAt(file, child.payload, buffer, count)) {
                            return false;
                        }
                        have_strh = parseStrh(buffer, count, stream);
                    } else if (child.id == fourcc('s', 't', 'r', 'f')) {
                        std::vector<uint8_t> buffer(child.size);
                        if (child.size > 0 && !readAt(file, child.payload, buffer.data(), buffer.size())) {
                            return false;
                        }
                        parseStrf(buffer.data(), buffer.size(), stream);
                    }
                    strl_pos = child.payload + static_cast<int64_t>(child.size) + (child.size & 1);
                }
                if (have_strh) {
                    streams.push_back(std::move(stream));
                }
            }
        }
        pos = chunk.payload + static_cast<int64_t>(chunk.size) + (chunk.size & 1);
    }
    return true;
}

// Returns 1 for a keyframe, 0 for a non-keyframe and -1 if the NAL header
// could not be inspected.
int inspectKeyframe(FILE* file, int64_t offset, uint32_t size, bool annexb, uint8_t length_size) {
    uint8_t buffer[24] = {};
    const size_t want = std::min<size_t>(size, sizeof(buffer));
    if (want < 3 || !readAt(file, offset, buffer, want)) {
        return -1;
    }
    if (annexb) {
        for (size_t i = 0; i + 2 < want; ++i) {
            if (buffer[i] == 0 && buffer[i + 1] == 0 && buffer[i + 2] == 1) {
                if (i + 3 >= want) {
                    return -1;
                }
                const uint8_t type = static_cast<uint8_t>(buffer[i + 3] >> 1);
                return (type >= 16 && type <= 23) ? 1 : 0;
            }
        }
        return -1;
    }
    if (length_size == 0 || length_size + 2 > want) {
        return -1;
    }
    uint32_t nal_size = 0;
    for (uint8_t i = 0; i < length_size; ++i) {
        nal_size = (nal_size << 8) | buffer[i];
    }
    if (nal_size < 2) {
        return -1;
    }
    const uint8_t type = static_cast<uint8_t>(buffer[length_size] >> 1);
    return (type >= 16 && type <= 23) ? 1 : 0;
}

bool looksAnnexB(FILE* file, int64_t offset, uint32_t size) {
    uint8_t buffer[4] = {};
    const size_t want = std::min<size_t>(size, 4);
    if (want < 3 || !readAt(file, offset, buffer, want)) {
        return false;
    }
    if (buffer[0] == 0 && buffer[1] == 0 && buffer[2] == 1) {
        return true;
    }
    return want == 4 && buffer[0] == 0 && buffer[1] == 0 && buffer[2] == 0 && buffer[3] == 1;
}

uint32_t audioFourcc(uint16_t format_tag) {
    if (format_tag == 0x00FF) return 0x6D703461;  // AAC -> 'mp4a'
    if (format_tag == 0x0055) return 0x6D703320;  // MP3 -> 'mp3 '
    return format_tag;
}

}  // namespace

AviDemuxer::AviDemuxer() = default;

AviDemuxer::~AviDemuxer() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool AviDemuxer::open(const std::filesystem::path& path, std::string& error) {
    file_ = std::fopen(path.string().c_str(), "rb");
    if (!file_) {
        error = "failed to open " + path.string();
        return false;
    }
    if (std::fseek(file_, 0, SEEK_END) != 0) {
        error = "failed to determine file size";
        return false;
    }
    const int64_t file_size = tellFile(file_);

    uint8_t riff[12] = {};
    if (file_size < 12 || !readAt(file_, 0, riff, 12) || std::memcmp(riff, "RIFF", 4) != 0 ||
        std::memcmp(riff + 8, "AVI ", 4) != 0) {
        error = "not a RIFF AVI file";
        return false;
    }

    std::vector<AviStream> streams;
    int64_t movi_data_start = -1;
    int64_t movi_data_size = 0;
    int64_t idx1_offset = -1;
    uint32_t idx1_size = 0;

    int64_t pos = 12;
    while (pos + 8 <= file_size) {
        if (!seekFile(file_, pos)) {
            break;
        }
        ChunkHeader chunk;
        if (!readChunkHeader(file_, chunk)) {
            break;
        }
        if (chunk.id == fourcc('L', 'I', 'S', 'T') && chunk.size >= 4) {
            uint8_t type_bytes[4] = {};
            if (!readAt(file_, chunk.payload, type_bytes, 4)) {
                break;
            }
            const uint32_t list_type = readU32(type_bytes);
            if (list_type == fourcc('h', 'd', 'r', 'l')) {
                if (!parseHdrl(file_, chunk.payload + 4,
                               static_cast<int64_t>(chunk.size) - 4, streams)) {
                    error = "failed to parse AVI header list";
                    return false;
                }
            } else if (list_type == fourcc('m', 'o', 'v', 'i')) {
                movi_data_start = chunk.payload + 4;
                movi_data_size = static_cast<int64_t>(chunk.size) - 4;
            }
        } else if (chunk.id == fourcc('i', 'd', 'x', '1')) {
            idx1_offset = chunk.payload;
            idx1_size = chunk.size;
        }
        pos = chunk.payload + static_cast<int64_t>(chunk.size) + (chunk.size & 1);
    }

    if (movi_data_start < 0) {
        error = "AVI movi list not found";
        return false;
    }

    // Select the first HEVC video stream.
    int video_stream_index = -1;
    for (size_t i = 0; i < streams.size(); ++i) {
        const AviStream& stream = streams[i];
        if (stream.fcc_type != fourcc('v', 'i', 'd', 's')) {
            continue;
        }
        const bool hevc_fourcc = isHevcFourcc(stream.fcc_handler) || isHevcFourcc(stream.compression);
        const bool hvcc_extradata =
            stream.extradata.size() >= 23 && stream.extradata[0] == 1;
        if (hevc_fourcc || hvcc_extradata) {
            video_stream_index = static_cast<int>(i);
            break;
        }
    }
    if (video_stream_index < 0) {
        error = "no HEVC video stream found in AVI";
        return false;
    }
    const AviStream& video_stream = streams[static_cast<size_t>(video_stream_index)];

    video_ = VideoTrackInfo{};
    video_.codec = Codec::Hevc;
    video_.timescale = 1000;
    video_.width = video_stream.width;
    video_.height = video_stream.height;

    bool annexb = false;
    uint8_t length_size = 4;
    if (video_stream.extradata.size() >= 23 && video_stream.extradata[0] == 1) {
        HvccInfo hvcc;
        std::string hvcc_error;
        if (parseHvcc(video_stream.extradata.data(), video_stream.extradata.size(), hvcc,
                      &hvcc_error)) {
            video_.hvcc = video_stream.extradata;
            length_size = hvcc.length_size;
        }
    }
    if (video_.hvcc.empty() && !video_stream.extradata.empty()) {
        static const uint8_t kStartCode[3] = {0, 0, 1};
        const bool has_start_code =
            std::search(video_stream.extradata.begin(), video_stream.extradata.end(),
                        kStartCode, kStartCode + 3) != video_stream.extradata.end();
        if (has_start_code) {
            annexb = true;
            annexb_prefix_ = video_stream.extradata;
        }
    }

    // Collect video chunk positions from idx1 or by scanning movi.
    struct ChunkRef {
        int64_t offset = 0;
        uint32_t size = 0;
        bool flagged_keyframe = false;
    };
    std::vector<ChunkRef> chunks;

    const uint8_t stream_digit_high = static_cast<uint8_t>('0' + (video_stream_index / 10) % 10);
    const uint8_t stream_digit_low = static_cast<uint8_t>('0' + video_stream_index % 10);
    const uint32_t video_chunk_id =
        (static_cast<uint32_t>(stream_digit_high) << 24) |
        (static_cast<uint32_t>(stream_digit_low) << 16) | (static_cast<uint32_t>('d') << 8) |
        static_cast<uint32_t>('c');

    if (idx1_offset >= 0 && idx1_size >= 16) {
        const uint32_t entry_count = idx1_size / 16;
        for (uint32_t i = 0; i < entry_count; ++i) {
            uint8_t entry[16] = {};
            if (!readAt(file_, idx1_offset + static_cast<int64_t>(i) * 16, entry, 16)) {
                break;
            }
            const uint32_t chunk_id = readU32(entry);
            const uint32_t flags = readU32(entry + 4);
            const uint32_t offset = readU32(entry + 8);
            if ((chunk_id & 0xFFFF0000u) != (video_chunk_id & 0xFFFF0000u)) {
                continue;
            }
            if ((chunk_id & 0xFFFFu) != fourcc(0, 0, 'd', 'c') &&
                (chunk_id & 0xFFFFu) != fourcc(0, 0, 'd', 'b')) {
                continue;
            }
            // The offset is relative to the movi data start, but some muxers
            // use the LIST chunk start instead; pick whichever matches.
            int64_t absolute = movi_data_start + offset;
            uint8_t probe[4] = {};
            if (!readAt(file_, absolute, probe, 4) || readU32(probe) != chunk_id) {
                const int64_t alternative = movi_data_start - 12 + offset;
                uint8_t probe2[4] = {};
                if (readAt(file_, alternative, probe2, 4) && readU32(probe2) == chunk_id) {
                    absolute = alternative;
                }
            }
            ChunkHeader header;
            if (!seekFile(file_, absolute) || !readChunkHeader(file_, header)) {
                continue;
            }
            if (header.size == 0 || header.payload + header.size > file_size) {
                continue;
            }
            chunks.push_back({header.payload, header.size, (flags & 0x10u) != 0});
        }
    } else {
        const int64_t movi_end = movi_data_start + movi_data_size;
        int64_t scan = movi_data_start;
        while (scan + 8 <= movi_end) {
            if (!seekFile(file_, scan)) {
                break;
            }
            ChunkHeader chunk;
            if (!readChunkHeader(file_, chunk)) {
                break;
            }
            if (chunk.id == video_chunk_id && chunk.size > 0 &&
                chunk.payload + chunk.size <= file_size) {
                chunks.push_back({chunk.payload, chunk.size, false});
            }
            scan = chunk.payload + static_cast<int64_t>(chunk.size) + (chunk.size & 1);
        }
    }

    if (chunks.empty()) {
        error = "no video chunks found in AVI movi list";
        return false;
    }

    if (!annexb && video_.hvcc.empty()) {
        annexb = looksAnnexB(file_, chunks.front().offset, chunks.front().size);
    }
    video_.format.annexb = annexb;
    video_.format.nal_length_size = length_size;

    if (video_stream.rate > 0 && video_stream.scale > 0) {
        fps_ = static_cast<double>(video_stream.rate) / video_stream.scale;
    }
    video_.fps = fps_;

    samples_.reserve(chunks.size());
    for (size_t i = 0; i < chunks.size(); ++i) {
        SampleInfo sample;
        sample.offset = chunks[i].offset;
        sample.size = chunks[i].size;
        const int inspected = inspectKeyframe(file_, chunks[i].offset, chunks[i].size, annexb,
                                              length_size);
        sample.keyframe = inspected == 1 || (inspected < 0 && chunks[i].flagged_keyframe);
        const double seconds = fps_ > 0.0 ? static_cast<double>(i) / fps_ : 0.0;
        sample.pts = static_cast<int64_t>(std::llround(seconds * 1000.0));
        samples_.push_back(sample);
    }

    for (size_t i = 0; i < samples_.size(); ++i) {
        if (samples_[i].keyframe) {
            keyframes_.emplace_back(samples_[i].pts, i);
        }
    }
    std::sort(keyframes_.begin(), keyframes_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    duration_ = fps_ > 0.0 ? static_cast<double>(samples_.size()) / fps_ : -1.0;

    for (const AviStream& stream : streams) {
        if (stream.fcc_type == fourcc('a', 'u', 'd', 's')) {
            audio_.present = true;
            audio_.codec_fourcc = audioFourcc(stream.audio_format_tag);
            audio_.channels = stream.audio_channels;
            audio_.sample_rate = stream.audio_sample_rate;
            break;
        }
    }

    next_sample_ = 0;
    return true;
}

bool AviDemuxer::readSample(Sample& sample) {
    if (!file_ || next_sample_ >= samples_.size()) {
        return false;
    }
    const SampleInfo& info = samples_[next_sample_];
    const size_t prefix =
        (info.keyframe && !annexb_prefix_.empty()) ? annexb_prefix_.size() : 0;
    sample.data.resize(prefix + info.size);
    if (prefix > 0) {
        std::memcpy(sample.data.data(), annexb_prefix_.data(), prefix);
    }
    if (info.size > 0 &&
        !readAt(file_, info.offset, sample.data.data() + prefix, info.size)) {
        return false;
    }
    sample.pts = info.pts;
    sample.dts = info.pts;
    sample.keyframe = info.keyframe;
    sample.track = TrackType::Video;
    ++next_sample_;
    return true;
}

bool AviDemuxer::seek(double seconds) {
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
