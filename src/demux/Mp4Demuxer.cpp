#include "demux/Mp4Demuxer.hpp"

#include "demux/HvcC.hpp"

#include <algorithm>
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

uint64_t readU64(const uint8_t* data) {
    return (static_cast<uint64_t>(readU32(data)) << 32) | readU32(data + 4);
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
    if (!seekFile(file, offset)) {
        return false;
    }
    return std::fread(out, 1, size, file) == size;
}

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool u8(uint8_t& value) {
        if (pos_ + 1 > size_) {
            return false;
        }
        value = data_[pos_++];
        return true;
    }
    bool u16(uint16_t& value) {
        if (pos_ + 2 > size_) {
            return false;
        }
        value = static_cast<uint16_t>((data_[pos_] << 8) | data_[pos_ + 1]);
        pos_ += 2;
        return true;
    }
    bool u24(uint32_t& value) {
        if (pos_ + 3 > size_) {
            return false;
        }
        value = (static_cast<uint32_t>(data_[pos_]) << 16) |
                (static_cast<uint32_t>(data_[pos_ + 1]) << 8) | data_[pos_ + 2];
        pos_ += 3;
        return true;
    }
    bool u32(uint32_t& value) {
        if (pos_ + 4 > size_) {
            return false;
        }
        value = readU32(data_ + pos_);
        pos_ += 4;
        return true;
    }
    bool u64(uint64_t& value) {
        if (pos_ + 8 > size_) {
            return false;
        }
        value = readU64(data_ + pos_);
        pos_ += 8;
        return true;
    }
    bool i32(int32_t& value) {
        uint32_t raw = 0;
        if (!u32(raw)) {
            return false;
        }
        value = static_cast<int32_t>(raw);
        return true;
    }
    bool skip(size_t count) {
        if (pos_ + count > size_) {
            return false;
        }
        pos_ += count;
        return true;
    }
    bool bytes(void* out, size_t count) {
        if (pos_ + count > size_) {
            return false;
        }
        std::memcpy(out, data_ + pos_, count);
        pos_ += count;
        return true;
    }
    const uint8_t* current() const { return data_ + pos_; }
    size_t remaining() const { return size_ - pos_; }
    size_t position() const { return pos_; }
    bool seek(size_t position) {
        if (position > size_) {
            return false;
        }
        pos_ = position;
        return true;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

struct BoxView {
    uint32_t type = 0;
    const uint8_t* payload = nullptr;
    size_t size = 0;
};

// Iterates the child boxes stored in data[0..size). pos is updated past the
// returned box. Returns false when there are no more complete boxes.
bool iterateBoxes(const uint8_t* data, size_t size, size_t& pos, BoxView& box) {
    if (pos + 8 > size) {
        return false;
    }
    uint64_t box_size = readU32(data + pos);
    const uint32_t type = readU32(data + pos + 4);
    size_t header_size = 8;
    if (box_size == 1) {
        if (pos + 16 > size) {
            return false;
        }
        box_size = readU64(data + pos + 8);
        header_size = 16;
    } else if (box_size == 0) {
        box_size = size - pos;
    }
    if (box_size < header_size || pos + box_size > size) {
        return false;
    }
    box.type = type;
    box.payload = data + pos + header_size;
    box.size = static_cast<size_t>(box_size) - header_size;
    pos += static_cast<size_t>(box_size);
    return true;
}

bool findChild(const uint8_t* data, size_t size, uint32_t type, BoxView& out) {
    size_t pos = 0;
    BoxView box;
    while (iterateBoxes(data, size, pos, box)) {
        if (box.type == type) {
            out = box;
            return true;
        }
    }
    return false;
}

bool parseMvhd(const BoxView& box, uint32_t& timescale) {
    Reader reader(box.payload, box.size);
    uint8_t version = 0;
    uint32_t flags = 0;
    if (!reader.u8(version) || !reader.u24(flags)) {
        return false;
    }
    if (version == 1) {
        if (!reader.skip(16) || !reader.u32(timescale)) {
            return false;
        }
    } else {
        if (!reader.skip(8) || !reader.u32(timescale)) {
            return false;
        }
    }
    return true;
}

bool parseTkhd(const BoxView& box, uint32_t& track_id, uint32_t& width, uint32_t& height) {
    Reader reader(box.payload, box.size);
    uint8_t version = 0;
    uint32_t flags = 0;
    if (!reader.u8(version) || !reader.u24(flags)) {
        return false;
    }
    if (version == 1) {
        if (!reader.skip(16) || !reader.u32(track_id) || !reader.skip(4 + 8 + 52)) {
            return false;
        }
    } else {
        if (!reader.skip(8) || !reader.u32(track_id) || !reader.skip(4 + 4 + 52)) {
            return false;
        }
    }
    uint32_t raw_width = 0;
    uint32_t raw_height = 0;
    if (!reader.u32(raw_width) || !reader.u32(raw_height)) {
        return false;
    }
    width = raw_width >> 16;
    height = raw_height >> 16;
    return true;
}

bool parseMdhd(const BoxView& box, uint32_t& timescale, uint64_t& duration) {
    Reader reader(box.payload, box.size);
    uint8_t version = 0;
    uint32_t flags = 0;
    if (!reader.u8(version) || !reader.u24(flags)) {
        return false;
    }
    if (version == 1) {
        if (!reader.skip(16) || !reader.u32(timescale) || !reader.u64(duration)) {
            return false;
        }
    } else {
        uint32_t duration32 = 0;
        if (!reader.skip(8) || !reader.u32(timescale) || !reader.u32(duration32)) {
            return false;
        }
        duration = duration32;
    }
    return true;
}

bool parseHdlr(const BoxView& box, uint32_t& handler) {
    Reader reader(box.payload, box.size);
    uint8_t version = 0;
    uint32_t flags = 0;
    if (!reader.u8(version) || !reader.u24(flags) || !reader.skip(4) || !reader.u32(handler)) {
        return false;
    }
    return true;
}

struct StblTables {
    uint32_t sample_count = 0;
    uint32_t constant_size = 0;
    std::vector<uint32_t> sizes;
    std::vector<int64_t> dts;
    std::vector<int64_t> ctts;
    std::vector<bool> sync;
    std::vector<uint32_t> sync_samples;  // 1-based sample numbers from stss
    bool has_stss = false;
    std::vector<uint64_t> chunk_offsets;
    struct StscEntry {
        uint32_t first_chunk;
        uint32_t samples_per_chunk;
    };
    std::vector<StscEntry> stsc;
};

bool parseStsz(const BoxView& box, StblTables& tables) {
    Reader reader(box.payload, box.size);
    uint8_t version = 0;
    uint32_t flags = 0;
    uint32_t sample_size = 0;
    uint32_t sample_count = 0;
    if (!reader.u8(version) || !reader.u24(flags) || !reader.u32(sample_size) ||
        !reader.u32(sample_count)) {
        return false;
    }
    if (sample_count > 10000000u) {
        return false;
    }
    tables.sample_count = sample_count;
    tables.constant_size = sample_size;
    if (sample_size == 0) {
        tables.sizes.resize(sample_count);
        for (uint32_t i = 0; i < sample_count; ++i) {
            if (!reader.u32(tables.sizes[i])) {
                return false;
            }
        }
    }
    return true;
}

bool parseStts(const BoxView& box, StblTables& tables) {
    Reader reader(box.payload, box.size);
    uint8_t version = 0;
    uint32_t flags = 0;
    uint32_t entry_count = 0;
    if (!reader.u8(version) || !reader.u24(flags) || !reader.u32(entry_count)) {
        return false;
    }
    int64_t dts = 0;
    for (uint32_t entry = 0; entry < entry_count; ++entry) {
        uint32_t count = 0;
        uint32_t delta = 0;
        if (!reader.u32(count) || !reader.u32(delta)) {
            return false;
        }
        for (uint32_t i = 0; i < count; ++i) {
            tables.dts.push_back(dts);
            dts += delta;
        }
    }
    return true;
}

bool parseCtts(const BoxView& box, StblTables& tables) {
    Reader reader(box.payload, box.size);
    uint8_t version = 0;
    uint32_t flags = 0;
    uint32_t entry_count = 0;
    if (!reader.u8(version) || !reader.u24(flags) || !reader.u32(entry_count)) {
        return false;
    }
    for (uint32_t entry = 0; entry < entry_count; ++entry) {
        uint32_t count = 0;
        uint32_t raw = 0;
        if (!reader.u32(count) || !reader.u32(raw)) {
            return false;
        }
        const int64_t offset = (version == 1) ? static_cast<int32_t>(raw) : static_cast<int64_t>(raw);
        for (uint32_t i = 0; i < count; ++i) {
            tables.ctts.push_back(offset);
        }
    }
    return true;
}

bool parseStss(const BoxView& box, StblTables& tables) {
    Reader reader(box.payload, box.size);
    uint8_t version = 0;
    uint32_t flags = 0;
    uint32_t entry_count = 0;
    if (!reader.u8(version) || !reader.u24(flags) || !reader.u32(entry_count)) {
        return false;
    }
    tables.has_stss = true;
    for (uint32_t entry = 0; entry < entry_count; ++entry) {
        uint32_t sample_number = 0;
        if (!reader.u32(sample_number)) {
            return false;
        }
        tables.sync_samples.push_back(sample_number);
    }
    return true;
}

bool parseStsc(const BoxView& box, StblTables& tables) {
    Reader reader(box.payload, box.size);
    uint8_t version = 0;
    uint32_t flags = 0;
    uint32_t entry_count = 0;
    if (!reader.u8(version) || !reader.u24(flags) || !reader.u32(entry_count)) {
        return false;
    }
    for (uint32_t entry = 0; entry < entry_count; ++entry) {
        uint32_t first_chunk = 0;
        uint32_t samples_per_chunk = 0;
        uint32_t description_index = 0;
        if (!reader.u32(first_chunk) || !reader.u32(samples_per_chunk) ||
            !reader.u32(description_index)) {
            return false;
        }
        tables.stsc.push_back({first_chunk, samples_per_chunk});
    }
    return true;
}

bool parseStco(const BoxView& box, bool is64, StblTables& tables) {
    Reader reader(box.payload, box.size);
    uint8_t version = 0;
    uint32_t flags = 0;
    uint32_t entry_count = 0;
    if (!reader.u8(version) || !reader.u24(flags) || !reader.u32(entry_count)) {
        return false;
    }
    tables.chunk_offsets.resize(entry_count);
    for (uint32_t entry = 0; entry < entry_count; ++entry) {
        if (is64) {
            if (!reader.u64(tables.chunk_offsets[entry])) {
                return false;
            }
        } else {
            uint32_t offset = 0;
            if (!reader.u32(offset)) {
                return false;
            }
            tables.chunk_offsets[entry] = offset;
        }
    }
    return true;
}

struct VideoSampleEntry {
    bool found = false;
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> hvcc;
};

bool parseStsd(const BoxView& box, VideoSampleEntry& video_entry, AudioTrackInfo& audio_info) {
    Reader reader(box.payload, box.size);
    uint8_t version = 0;
    uint32_t flags = 0;
    uint32_t entry_count = 0;
    if (!reader.u8(version) || !reader.u24(flags) || !reader.u32(entry_count)) {
        return false;
    }
    for (uint32_t entry = 0; entry < entry_count; ++entry) {
        if (reader.remaining() < 8) {
            return false;
        }
        const uint8_t* entry_start = reader.current();
        uint32_t entry_size = readU32(entry_start);
        const uint32_t format = readU32(entry_start + 4);
        if (entry_size < 8 || entry_size > reader.remaining()) {
            return false;
        }
        const uint8_t* payload = entry_start + 8;
        const size_t payload_size = entry_size - 8;

        if ((format == fourcc('h', 'v', 'c', '1') || format == fourcc('h', 'e', 'v', '1')) &&
            !video_entry.found && payload_size >= 78) {
            video_entry.found = true;
            video_entry.width = static_cast<uint32_t>((payload[24] << 8) | payload[25]);
            video_entry.height = static_cast<uint32_t>((payload[26] << 8) | payload[27]);
            // VisualSampleEntry fixed fields are 78 bytes; children follow.
            size_t child_pos = 78;
            BoxView child;
            while (iterateBoxes(payload, payload_size, child_pos, child)) {
                if (child.type == fourcc('h', 'v', 'c', 'C')) {
                    video_entry.hvcc.assign(child.payload, child.payload + child.size);
                }
            }
        } else if (payload_size >= 28) {
            // AudioSampleEntry: only track info is needed for now.
            const uint16_t sample_entry_version = static_cast<uint16_t>((payload[8] << 8) | payload[9]);
            const uint16_t channels = static_cast<uint16_t>((payload[16] << 8) | payload[17]);
            const uint32_t sample_rate = readU32(payload + 24) >> 16;
            if (!audio_info.present) {
                audio_info.present = true;
                audio_info.codec_fourcc = format;
                audio_info.channels = channels;
                audio_info.sample_rate = sample_rate;
                (void)sample_entry_version;
            }
        }
        reader.skip(entry_size);
    }
    return true;
}

bool parseStbl(const BoxView& box, VideoSampleEntry& video_entry, AudioTrackInfo& audio_info,
               StblTables& tables) {
    size_t pos = 0;
    BoxView child;
    bool have_stsz = false;
    bool have_stts = false;
    bool have_stco = false;
    while (iterateBoxes(box.payload, box.size, pos, child)) {
        if (child.type == fourcc('s', 't', 's', 'd')) {
            if (!parseStsd(child, video_entry, audio_info)) {
                return false;
            }
        } else if (child.type == fourcc('s', 't', 's', 'z')) {
            if (!parseStsz(child, tables)) {
                return false;
            }
            have_stsz = true;
        } else if (child.type == fourcc('s', 't', 't', 's')) {
            if (!parseStts(child, tables)) {
                return false;
            }
            have_stts = true;
        } else if (child.type == fourcc('c', 't', 't', 's')) {
            if (!parseCtts(child, tables)) {
                return false;
            }
        } else if (child.type == fourcc('s', 't', 's', 's')) {
            if (!parseStss(child, tables)) {
                return false;
            }
        } else if (child.type == fourcc('s', 't', 's', 'c')) {
            if (!parseStsc(child, tables)) {
                return false;
            }
        } else if (child.type == fourcc('s', 't', 'c', 'o')) {
            if (!parseStco(child, false, tables)) {
                return false;
            }
            have_stco = true;
        } else if (child.type == fourcc('c', 'o', '6', '4')) {
            if (!parseStco(child, true, tables)) {
                return false;
            }
            have_stco = true;
        }
    }
    // stss is stored before stsz in the usual box order, so the sync table can
    // only be resolved once the sample count is known.
    if (tables.has_stss && have_stsz) {
        tables.sync.assign(tables.sample_count, false);
        for (uint32_t sample_number : tables.sync_samples) {
            if (sample_number >= 1 && sample_number <= tables.sample_count) {
                tables.sync[sample_number - 1] = true;
            }
        }
    }
    return have_stsz && have_stts && have_stco;
}

struct TrackResult {
    bool is_video = false;
    bool is_audio = false;
    bool stbl_ok = false;
    uint32_t timescale = 0;
    uint64_t duration = 0;
    uint32_t tkhd_width = 0;
    uint32_t tkhd_height = 0;
    VideoSampleEntry video_entry;
    AudioTrackInfo audio_info;
    StblTables tables;
};

bool parseTrack(const BoxView& trak, TrackResult& result) {
    size_t pos = 0;
    BoxView child;
    while (iterateBoxes(trak.payload, trak.size, pos, child)) {
        if (child.type == fourcc('t', 'k', 'h', 'd')) {
            uint32_t track_id = 0;
            if (!parseTkhd(child, track_id, result.tkhd_width, result.tkhd_height)) {
                return false;
            }
        } else if (child.type == fourcc('m', 'd', 'i', 'a')) {
            size_t media_pos = 0;
            BoxView media_child;
            while (iterateBoxes(child.payload, child.size, media_pos, media_child)) {
                if (media_child.type == fourcc('m', 'd', 'h', 'd')) {
                    if (!parseMdhd(media_child, result.timescale, result.duration)) {
                        return false;
                    }
                } else if (media_child.type == fourcc('h', 'd', 'l', 'r')) {
                    uint32_t handler = 0;
                    if (!parseHdlr(media_child, handler)) {
                        return false;
                    }
                    result.is_video = handler == fourcc('v', 'i', 'd', 'e');
                    result.is_audio = handler == fourcc('s', 'o', 'u', 'n');
                } else if (media_child.type == fourcc('m', 'i', 'n', 'f')) {
                    BoxView stbl;
                    if (findChild(media_child.payload, media_child.size, fourcc('s', 't', 'b', 'l'), stbl)) {
                        result.stbl_ok = parseStbl(stbl, result.video_entry, result.audio_info, result.tables);
                    }
                }
            }
        }
    }
    return true;
}

bool buildSamples(const StblTables& tables, std::vector<Mp4Demuxer::SampleInfo>& samples,
                  std::vector<std::pair<int64_t, size_t>>& keyframes, std::string& error) {
    const uint32_t count = tables.sample_count;
    if (count == 0) {
        error = "video track has no samples";
        return false;
    }
    if (tables.dts.size() < count || tables.chunk_offsets.empty() || tables.stsc.empty()) {
        error = "incomplete MP4 sample tables";
        return false;
    }

    std::vector<uint32_t> sizes;
    if (tables.constant_size != 0) {
        sizes.assign(count, tables.constant_size);
    } else {
        if (tables.sizes.size() < count) {
            error = "stsz sample count mismatch";
            return false;
        }
        sizes = tables.sizes;
    }

    samples.assign(count, Mp4Demuxer::SampleInfo{});
    size_t stsc_index = 0;
    size_t sample = 0;
    for (size_t chunk = 0; chunk < tables.chunk_offsets.size() && sample < count; ++chunk) {
        while (stsc_index + 1 < tables.stsc.size() &&
               tables.stsc[stsc_index + 1].first_chunk <= chunk + 1) {
            ++stsc_index;
        }
        const uint32_t samples_per_chunk = tables.stsc[stsc_index].samples_per_chunk;
        uint64_t offset = tables.chunk_offsets[chunk];
        for (uint32_t i = 0; i < samples_per_chunk && sample < count; ++i) {
            samples[sample].offset = offset;
            samples[sample].size = sizes[sample];
            offset += sizes[sample];
            ++sample;
        }
    }
    if (sample != count) {
        error = "MP4 chunk table does not cover all samples";
        return false;
    }

    if (tables.has_stss && tables.sync.size() < count) {
        error = "stss sample count mismatch";
        return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
        samples[i].dts = tables.dts[i];
        const int64_t ctts = (i < tables.ctts.size()) ? tables.ctts[i] : 0;
        samples[i].pts = samples[i].dts + ctts;
        samples[i].keyframe = tables.has_stss ? tables.sync[i] : true;
    }

    keyframes.clear();
    for (size_t i = 0; i < samples.size(); ++i) {
        if (samples[i].keyframe) {
            keyframes.emplace_back(samples[i].pts, i);
        }
    }
    std::sort(keyframes.begin(), keyframes.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return true;
}

bool findTopLevelBox(FILE* file, int64_t file_size, uint32_t wanted, uint64_t& payload_offset,
                     uint64_t& payload_size, std::string& error) {
    int64_t pos = 0;
    uint8_t header[16] = {};
    while (pos + 8 <= file_size) {
        if (!readAt(file, pos, header, 8)) {
            error = "failed to read MP4 box header";
            return false;
        }
        uint64_t box_size = readU32(header);
        const uint32_t type = readU32(header + 4);
        size_t header_size = 8;
        if (box_size == 1) {
            if (!readAt(file, pos + 8, header + 8, 8)) {
                error = "failed to read large MP4 box size";
                return false;
            }
            box_size = readU64(header + 8);
            header_size = 16;
        } else if (box_size == 0) {
            box_size = static_cast<uint64_t>(file_size - pos);
        }
        if (box_size < header_size || pos + static_cast<int64_t>(box_size) > file_size) {
            error = "malformed MP4 box structure";
            return false;
        }
        if (type == wanted) {
            payload_offset = static_cast<uint64_t>(pos) + header_size;
            payload_size = box_size - header_size;
            return true;
        }
        pos += static_cast<int64_t>(box_size);
    }
    error = "moov box not found (fragmented MP4 is not supported yet)";
    return false;
}

}  // namespace

Mp4Demuxer::Mp4Demuxer() = default;

Mp4Demuxer::~Mp4Demuxer() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

bool Mp4Demuxer::open(const std::filesystem::path& path, std::string& error) {
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

    uint64_t moov_offset = 0;
    uint64_t moov_size = 0;
    if (!findTopLevelBox(file_, file_size, fourcc('m', 'o', 'o', 'v'), moov_offset, moov_size, error)) {
        return false;
    }
    if (moov_size > 256u * 1024u * 1024u) {
        error = "moov box is unreasonably large";
        return false;
    }

    std::vector<uint8_t> moov(static_cast<size_t>(moov_size));
    if (!readAt(file_, static_cast<int64_t>(moov_offset), moov.data(), moov.size())) {
        error = "failed to read moov box";
        return false;
    }

    uint32_t movie_timescale = 0;
    BoxView mvhd;
    if (findChild(moov.data(), moov.size(), fourcc('m', 'v', 'h', 'd'), mvhd)) {
        parseMvhd(mvhd, movie_timescale);
    }
    (void)movie_timescale;

    TrackResult video_track;
    TrackResult audio_track;
    bool have_video = false;
    bool have_audio = false;

    size_t pos = 0;
    BoxView box;
    while (iterateBoxes(moov.data(), moov.size(), pos, box)) {
        if (box.type != fourcc('t', 'r', 'a', 'k')) {
            continue;
        }
        TrackResult track;
        if (!parseTrack(box, track)) {
            error = "failed to parse MP4 track";
            return false;
        }
        if (track.is_video && track.video_entry.found && track.stbl_ok && !have_video) {
            video_track = std::move(track);
            have_video = true;
        } else if (track.is_audio && !have_audio) {
            audio_track = std::move(track);
            have_audio = true;
        }
    }

    if (!have_video) {
        error = "no HEVC video track found in MP4";
        return false;
    }

    video_ = VideoTrackInfo{};
    video_.codec = Codec::Hevc;
    video_.timescale = video_track.timescale != 0 ? video_track.timescale : 1000;
    video_.format.annexb = false;
    video_.format.nal_length_size = 4;
    video_.width = video_track.video_entry.width != 0 ? video_track.video_entry.width
                                                     : video_track.tkhd_width;
    video_.height = video_track.video_entry.height != 0 ? video_track.video_entry.height
                                                        : video_track.tkhd_height;
    video_.hvcc = video_track.video_entry.hvcc;

    if (!video_.hvcc.empty()) {
        HvccInfo hvcc;
        std::string hvcc_error;
        if (parseHvcc(video_.hvcc.data(), video_.hvcc.size(), hvcc, &hvcc_error)) {
            video_.format.nal_length_size = hvcc.length_size;
        }
    }

    if (!buildSamples(video_track.tables, samples_, keyframes_, error)) {
        return false;
    }

    if (video_track.duration != 0 && video_.timescale != 0) {
        duration_ = static_cast<double>(video_track.duration) / video_.timescale;
        video_.fps = static_cast<double>(samples_.size()) * video_.timescale /
                     static_cast<double>(video_track.duration);
    } else {
        const int64_t last = samples_.back().dts;
        const int64_t first = samples_.front().dts;
        duration_ = static_cast<double>(last - first) / video_.timescale;
        video_.fps = duration_ > 0.0 ? samples_.size() / duration_ : 0.0;
    }

    if (have_audio) {
        audio_ = audio_track.audio_info;
    }

    next_sample_ = 0;
    return true;
}

bool Mp4Demuxer::readSample(Sample& sample) {
    if (!file_ || next_sample_ >= samples_.size()) {
        return false;
    }
    const SampleInfo& info = samples_[next_sample_];
    sample.data.resize(info.size);
    if (info.size > 0 && !readAt(file_, static_cast<int64_t>(info.offset), sample.data.data(), info.size)) {
        return false;
    }
    sample.pts = info.pts;
    sample.dts = info.dts;
    sample.keyframe = info.keyframe;
    sample.track = TrackType::Video;
    ++next_sample_;
    return true;
}

bool Mp4Demuxer::seek(double seconds) {
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
