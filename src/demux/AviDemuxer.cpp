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

// RIFF stores fourcc tags as ASCII byte sequences and all other values as
// little-endian integers.
uint32_t readU32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | static_cast<uint32_t>(data[3]);
}

uint32_t readU32Le(const uint8_t* data) {
    return static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
           (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
}

uint16_t readU16Le(const uint8_t* data) {
    return static_cast<uint16_t>(data[0] | (data[1] << 8));
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
    out.size = readU32Le(header + 4);
    out.payload = tellFile(file);
    return true;
}

// VFW fourccs are not case sensitive: ffmpeg writes lowercase "hevc" while
// other muxers use "HEVC" or "H265".
bool isHevcFourcc(uint32_t value) {
    return value == fourcc('H', 'E', 'V', 'C') || value == fourcc('h', 'e', 'v', 'c') ||
           value == fourcc('H', '2', '6', '5') || value == fourcc('h', 'v', 'c', '1') ||
           value == fourcc('h', 'e', 'v', '1') || value == fourcc('x', '2', '6', '5');
}

// Fourccs VFW uses for MPEG-1 video.
bool isMpeg1Fourcc(uint32_t value) {
    return value == fourcc('m', 'p', 'g', '1') || value == fourcc('M', 'P', 'G', '1') ||
           value == fourcc('P', 'I', 'M', '1') || value == fourcc('B', 'W', '1', '0') ||
           value == fourcc('X', 'M', 'P', 'G') ||
           value == 0x10000001u;  // the numeric tag 1,0,0,16
}

// Fourccs VFW uses for MPEG-2 video.
bool isMpeg2Fourcc(uint32_t value) {
    return value == fourcc('m', 'p', 'g', '2') || value == fourcc('M', 'P', 'G', '2') ||
           value == fourcc('M', 'P', 'E', 'G') || value == fourcc('P', 'I', 'M', '2') ||
           value == fourcc('V', 'C', 'R', '2') || value == fourcc('D', 'V', 'R', ' ') ||
           value == fourcc('M', 'M', 'E', 'S') || value == fourcc('L', 'M', 'P', '2') ||
           value == fourcc('s', 'l', 'i', 'f') || value == fourcc('E', 'M', '2', 'V') ||
           value == fourcc('M', '7', '0', '1') || value == fourcc('M', '7', '0', '2') ||
           value == fourcc('M', '7', '0', '3') || value == fourcc('M', '7', '0', '4') ||
           value == fourcc('M', '7', '0', '5') || value == fourcc('m', 'p', 'g', 'v') ||
           value == 0x10000002u;  // the numeric tag 2,0,0,16
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
    uint16_t audio_block_align = 0;
    uint16_t audio_bits = 0;
};

bool parseStrh(const uint8_t* data, size_t size, AviStream& stream) {
    if (size < 40) {
        return false;
    }
    stream.fcc_type = readU32(data);
    stream.fcc_handler = readU32(data + 4);
    stream.scale = readU32Le(data + 20);
    stream.rate = readU32Le(data + 24);
    stream.length = readU32Le(data + 32);
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
        stream.width = readU32Le(data + 4);
        const int32_t height = static_cast<int32_t>(readU32Le(data + 8));
        stream.height = height < 0 ? static_cast<uint32_t>(-height) : static_cast<uint32_t>(height);
        stream.compression = readU32(data + 16);
        if (size > 40) {
            stream.extradata.assign(data + 40, data + size);
        }
    } else if (stream.fcc_type == fourcc('a', 'u', 'd', 's')) {
        // WAVEFORMATEX: tag, channels, rate, average rate, block align, bits
        // and the trailing codec specific bytes.
        stream.audio_format_tag = readU16Le(data);
        stream.audio_channels = readU16Le(data + 2);
        stream.audio_sample_rate = readU32Le(data + 4);
        stream.audio_block_align = readU16Le(data + 12);
        stream.audio_bits = readU16Le(data + 14);
        if (size >= 18) {
            const uint16_t extra_size = readU16Le(data + 16);
            const size_t available = size - 18;
            // Some writers leave cbSize at zero while still appending data.
            const size_t take = extra_size != 0 ? std::min<size_t>(extra_size, available)
                                                : available;
            if (take > 0) {
                stream.extradata.assign(data + 18, data + 18 + take);
            }
        }
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

// FFmpeg and some other muxers store Annex B parameter sets as AVI codec
// extradata instead of an hvcC record.
bool looksLikeAnnexBHevc(const std::vector<uint8_t>& data) {
    for (size_t i = 0; i + 3 < data.size(); ++i) {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1) {
            const uint8_t type = static_cast<uint8_t>(data[i + 3] >> 1);
            if (type <= 34) {
                return true;
            }
        }
    }
    return false;
}

// Maps a WAVEFORMATEX tag to the fourcc the audio decoders are keyed by. Only
// tags with a matching decoder are translated; anything else keeps its raw tag
// so the log can name it.
uint32_t audioFourcc(uint16_t format_tag) {
    switch (format_tag) {
        case 0x0055:
            return 0x6D703320;  // MPEG audio -> 'mp3 '
        case 0x00FF:
            return 0x6D703461;  // AAC -> 'mp4a'
        case 0x2000:
            return 0x61632D33;  // AC-3 -> 'ac-3'
        case 0x704F:
            return 0x4F707573;  // Opus -> 'Opus'
        case 0x0001:            // integer PCM
        case 0x0003:            // IEEE float PCM
        case 0x0006:
        case 0x0010:
            return 0x72617720;  // 'raw '
        default:
            return format_tag;
    }
}

// Codecs whose decoder configuration lives in the WAVEFORMATEX trailing bytes:
// AAC carries its AudioSpecificConfig there, Opus a complete OpusHead.
bool audioUsesExtradata(uint16_t format_tag) {
    return format_tag == 0x00FF || format_tag == 0x704F;
}

std::string fourccText(uint32_t value) {
    std::string text;
    for (int i = 3; i >= 0; --i) {
        const char letter = static_cast<char>((value >> (i * 8)) & 0xFF);
        text.push_back(letter >= 0x20 && letter < 0x7F ? letter : '?');
    }
    return text;
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

    // Select the first HEVC, MPEG-2 or MPEG-1 video stream.
    int video_stream_index = -1;
    Codec video_codec = Codec::Unknown;
    for (size_t i = 0; i < streams.size(); ++i) {
        const AviStream& stream = streams[i];
        if (stream.fcc_type != fourcc('v', 'i', 'd', 's')) {
            continue;
        }
        const bool hevc_fourcc = isHevcFourcc(stream.fcc_handler) || isHevcFourcc(stream.compression);
        const bool mpeg1_fourcc =
            isMpeg1Fourcc(stream.fcc_handler) || isMpeg1Fourcc(stream.compression);
        const bool mpeg2_fourcc =
            isMpeg2Fourcc(stream.fcc_handler) || isMpeg2Fourcc(stream.compression);
        const bool hvcc_extradata =
            stream.extradata.size() >= 23 && stream.extradata[0] == 1;
        if (hevc_fourcc || hvcc_extradata || looksLikeAnnexBHevc(stream.extradata)) {
            video_stream_index = static_cast<int>(i);
            video_codec = Codec::Hevc;
            break;
        }
        if (mpeg2_fourcc) {
            video_stream_index = static_cast<int>(i);
            video_codec = Codec::Mpeg2;
            break;
        }
        if (mpeg1_fourcc) {
            video_stream_index = static_cast<int>(i);
            video_codec = Codec::Mpeg1;
            break;
        }
    }
    if (video_stream_index < 0) {
        error = "no HEVC, MPEG-2 or MPEG-1 video stream found in AVI";
        return false;
    }
    const AviStream& video_stream = streams[static_cast<size_t>(video_stream_index)];

    video_ = VideoTrackInfo{};
    video_.codec = video_codec;
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

    // Collect chunk positions from idx1 or by scanning movi.
    struct ChunkRef {
        int64_t offset = 0;
        uint32_t size = 0;
        bool flagged_keyframe = false;
    };

    const auto chunk_id_for = [](uint32_t stream_number) {
        return (static_cast<uint32_t>('0' + (stream_number / 10) % 10) << 24) |
               (static_cast<uint32_t>('0' + stream_number % 10) << 16);
    };

    // Audio chunk ids only pin the stream number ("01wb"), while video chunks
    // use the "00dc"/"00db" compressed forms, so the caller says which ones it
    // accepts.
    const auto collect_chunks = [&](uint32_t stream_number, bool audio_stream) {
        std::vector<ChunkRef> found;
        const uint32_t stream_prefix = chunk_id_for(stream_number);

        if (idx1_offset >= 0 && idx1_size >= 16) {
            const uint32_t entry_count = idx1_size / 16;
            for (uint32_t i = 0; i < entry_count; ++i) {
                uint8_t entry[16] = {};
                if (!readAt(file_, idx1_offset + static_cast<int64_t>(i) * 16, entry, 16)) {
                    break;
                }
                const uint32_t chunk_id = readU32(entry);
                if ((chunk_id & 0xFFFF0000u) != stream_prefix) {
                    continue;
                }
                const uint16_t suffix = static_cast<uint16_t>(chunk_id & 0xFFFFu);
                if (!audio_stream && suffix != fourcc(0, 0, 'd', 'c') &&
                    suffix != fourcc(0, 0, 'd', 'b')) {
                    continue;
                }
                const uint32_t flags = readU32Le(entry + 4);
                const uint32_t offset = readU32Le(entry + 8);
                // Muxers disagree on the idx1 offset base: after the 'movi'
                // fourcc, at the 'movi' fourcc, or at the LIST header. Probe all
                // three and keep the one where the chunk id actually matches.
                const int64_t candidates[3] = {
                    movi_data_start + offset,
                    movi_data_start - 4 + offset,
                    movi_data_start - 12 + offset,
                };
                int64_t absolute = candidates[0];
                for (int64_t candidate : candidates) {
                    uint8_t probe[4] = {};
                    if (candidate >= 0 && readAt(file_, candidate, probe, 4) &&
                        readU32(probe) == chunk_id) {
                        absolute = candidate;
                        break;
                    }
                }
                ChunkHeader header;
                if (!seekFile(file_, absolute) || !readChunkHeader(file_, header)) {
                    continue;
                }
                if (header.size == 0 || header.payload + header.size > file_size) {
                    continue;
                }
                found.push_back({header.payload, header.size, (flags & 0x10u) != 0});
            }
            return found;
        }

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
            const bool same_stream = (chunk.id & 0xFFFF0000u) == stream_prefix;
            const bool usable = audio_stream ||
                                (chunk.id & 0xFFFFu) == fourcc(0, 0, 'd', 'c') ||
                                (chunk.id & 0xFFFFu) == fourcc(0, 0, 'd', 'b');
            if (same_stream && usable && chunk.size > 0 &&
                chunk.payload + chunk.size <= file_size) {
                found.push_back({chunk.payload, chunk.size, false});
            }
            scan = chunk.payload + static_cast<int64_t>(chunk.size) + (chunk.size & 1);
        }
        return found;
    };

    const std::vector<ChunkRef> chunks =
        collect_chunks(static_cast<uint32_t>(video_stream_index), false);

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

    // Audio streams: AVI carries no per packet timing, so the presentation
    // times follow from the stream header scale and rate.
    for (size_t i = 0; i < streams.size(); ++i) {
        const AviStream& stream = streams[i];
        if (stream.fcc_type != fourcc('a', 'u', 'd', 's') || stream.rate == 0 ||
            stream.scale == 0) {
            continue;
        }
        const std::vector<ChunkRef> audio_chunks = collect_chunks(static_cast<uint32_t>(i), true);
        if (audio_chunks.empty()) {
            continue;
        }
        AudioTrackInfo info;
        info.index = audio_tracks_.size();
        info.codec_fourcc = audioFourcc(stream.audio_format_tag);
        info.codec = fourccText(info.codec_fourcc);
        info.present = true;
        info.default_track = audio_tracks_.empty();
        info.channels = stream.audio_channels;
        info.sample_rate = stream.audio_sample_rate;
        if (audioUsesExtradata(stream.audio_format_tag)) {
            info.extradata = stream.extradata;
        }

        TrackSamples track;
        track.samples.reserve(audio_chunks.size());
        for (size_t c = 0; c < audio_chunks.size(); ++c) {
            SampleInfo packet;
            packet.offset = audio_chunks[c].offset;
            packet.size = audio_chunks[c].size;
            packet.keyframe = true;
            packet.pts = static_cast<int64_t>(c) * stream.scale * 1000 / stream.rate;
            track.samples.push_back(packet);
        }
        audio_tracks_.push_back(std::move(info));
        audio_samples_.push_back(std::move(track));
    }
    if (!audio_tracks_.empty()) {
        audio_ = audio_tracks_.front();
    }

    next_sample_ = 0;
    return true;
}

bool AviDemuxer::readAudioSample(Sample& sample) {
    if (!file_ || selected_audio_ >= audio_samples_.size()) {
        return false;
    }
    TrackSamples& track = audio_samples_[selected_audio_];
    if (track.next >= track.samples.size()) {
        return false;
    }
    const SampleInfo& info = track.samples[track.next];
    sample.data.resize(info.size);
    if (info.size > 0 &&
        !readAt(file_, info.offset, sample.data.data(), info.size)) {
        return false;
    }
    sample.pts = info.pts;
    sample.dts = info.pts;
    sample.keyframe = true;
    sample.track = TrackType::Audio;
    ++track.next;
    return true;
}

bool AviDemuxer::selectAudioTrack(size_t index) {
    if (index >= audio_tracks_.size() || index >= audio_samples_.size()) {
        return false;
    }
    selected_audio_ = index;
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

    // The audio cursors are independent of the video one, so they have to move
    // as well. Left ahead of the new playhead they hand the player packets from
    // the future, which the sink then refuses to play.
    for (TrackSamples& track : audio_samples_) {
        // Step back to the packet that covers the target for a short lead.
        const auto after = std::upper_bound(
            track.samples.begin(), track.samples.end(), target,
            [](int64_t value, const SampleInfo& info) { return value < info.pts; });
        track.next = after == track.samples.begin()
                         ? 0
                         : static_cast<size_t>(std::prev(after) - track.samples.begin());
    }
    return true;
}

}  // namespace player::demux
