#include "demux/Mp4Demuxer.hpp"

#include "demux/Esds.hpp"
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

std::string fourccText(uint32_t value) {
    std::string text;
    for (int i = 3; i >= 0; --i) {
        const char letter = static_cast<char>((value >> (i * 8)) & 0xFF);
        text.push_back(letter >= 0x20 && letter < 0x7F ? letter : '?');
    }
    return text;
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

// mdhd packs a three letter code into 15 bits, each letter biased by 0x60.
std::string isoLanguage(uint16_t packed) {
    std::string text;
    for (int i = 2; i >= 0; --i) {
        const uint16_t letter = (packed >> (i * 5)) & 0x1F;
        text.push_back(letter >= 0x01 && letter <= 0x1A ? static_cast<char>(letter + 0x60)
                                                        : '?');
    }
    return text == "???" ? std::string() : text;
}

bool parseMdhd(const BoxView& box, uint32_t& timescale, uint64_t& duration,
               std::string& language) {
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
    uint16_t packed_language = 0;
    if (reader.u16(packed_language)) {
        language = isoLanguage(packed_language);
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
    std::vector<int64_t> durations;
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
            tables.durations.push_back(delta);
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

// The single sample entry that matters for a track: one of the video, audio or
// timed text kinds the player can act on.
struct SampleEntry {
    bool found = false;
    uint32_t format = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t channels = 0;
    uint32_t sample_rate = 0;
    std::vector<uint8_t> hvcc;
    std::vector<uint8_t> esds;
    std::vector<uint8_t> codec_private;
};

// Walks the esds descriptor chain for the decoder specific info, which is the
// AudioSpecificConfig the AAC decoder expects as its configuration.
bool findDecoderSpecificInfo(const uint8_t* data, size_t size, std::vector<uint8_t>& out) {
    size_t pos = 0;
    while (pos < size) {
        const uint8_t tag = data[pos++];
        uint64_t length = 0;
        for (int byte = 0; byte < 4; ++byte) {
            if (pos >= size) {
                return false;
            }
            const uint8_t value = data[pos++];
            length = (length << 7) | (value & 0x7F);
            if ((value & 0x80) == 0) {
                break;
            }
        }
        if (pos + length > size) {
            return false;
        }
        if (tag == 0x05) {  // DecoderSpecificInfo
            out.assign(data + pos, data + pos + length);
            return true;
        }
        if (tag == 0x03 || tag == 0x04) {
            // ES_Descriptor and DecoderConfigDescriptor have a fixed header
            // before their children.
            const size_t header = tag == 0x03 ? 3 : 13;
            if (length < header) {
                return false;
            }
            if (findDecoderSpecificInfo(data + pos + header, length - header, out)) {
                return true;
            }
        }
        pos += length;
    }
    return false;
}

// dOps carries the OpusHead fields big endian while OpusHead itself is little
// endian, so every multi byte field has to be swapped while rebuilding it.
std::vector<uint8_t> opusHeadFromDops(const uint8_t* data, size_t size) {
    std::vector<uint8_t> head = {'O', 'p', 'u', 's', 'H', 'e', 'a', 'd'};
    constexpr size_t kFixedFields = 11;  // version, channels, pre-skip, rate, gain, family
    if (size < kFixedFields) {
        return head;
    }
    head.push_back(data[0]);  // version
    head.push_back(data[1]);  // channel count
    head.push_back(data[3]);
    head.push_back(data[2]);  // pre-skip
    for (size_t i = 7; i >= 4; --i) {
        head.push_back(data[i]);  // input sample rate
    }
    head.push_back(data[9]);
    head.push_back(data[8]);  // output gain
    head.push_back(data[10]);  // mapping family
    // The channel mapping table is a byte list, so it keeps its order.
    head.insert(head.end(), data + kFixedFields, data + size);
    return head;
}

// AudioSampleEntry grew two extra fields per revision; children start after
// them.
size_t audioEntryChildOffset(const uint8_t* payload, size_t payload_size) {
    if (payload_size < 28) {
        return 0;
    }
    const uint16_t version = static_cast<uint16_t>((payload[8] << 8) | payload[9]);
    size_t offset = 28;
    if (version == 1) {
        offset += 16;
    } else if (version == 2) {
        offset += 36;
    }
    return offset <= payload_size ? offset : 0;
}

bool parseStsd(const BoxView& box, SampleEntry& entry) {
    Reader reader(box.payload, box.size);
    uint8_t version = 0;
    uint32_t flags = 0;
    uint32_t entry_count = 0;
    if (!reader.u8(version) || !reader.u24(flags) || !reader.u32(entry_count)) {
        return false;
    }
    for (uint32_t i = 0; i < entry_count; ++i) {
        if (reader.remaining() < 8) {
            return false;
        }
        const uint8_t* entry_start = reader.current();
        const uint32_t entry_size = readU32(entry_start);
        const uint32_t format = readU32(entry_start + 4);
        if (entry_size < 8 || entry_size > reader.remaining()) {
            return false;
        }
        const uint8_t* payload = entry_start + 8;
        const size_t payload_size = entry_size - 8;
        if (entry.found) {
            reader.skip(entry_size);
            continue;
        }

        const bool is_hevc = format == fourcc('h', 'v', 'c', '1') ||
                             format == fourcc('h', 'e', 'v', '1');
        // `mp4v` carries MPEG-1, MPEG-2 or MPEG-4 video; the esds descriptor
        // below tells them apart.
        const bool is_mpeg_family = format == fourcc('m', 'p', '4', 'v');
        const bool is_video = is_hevc || is_mpeg_family;
        const bool is_text = format == fourcc('t', 'x', '3', 'g') ||
                             format == fourcc('w', 'v', 't', 't') ||
                             format == fourcc('s', 'b', 't', 'l') ||
                             format == fourcc('t', 'e', 'x', 't');
        const bool is_audio = format == fourcc('m', 'p', '4', 'a') ||
                              format == fourcc('O', 'p', 'u', 's') ||
                              format == fourcc('a', 'c', '-', '3') ||
                              format == fourcc('e', 'c', '-', '3') ||
                              format == fourcc('a', 'l', 'a', 'c') ||
                              format == fourcc('s', 'a', 'm', 'r') ||
                              format == fourcc('s', 'a', 'w', 'b') ||
                              format == fourcc('t', 'w', 'o', 's') ||
                              format == fourcc('d', 't', 's', 'c') ||
                              format == fourcc('d', 't', 's', 'h');

        if (is_video && payload_size >= 78) {
            entry.found = true;
            entry.format = format;
            entry.width = static_cast<uint32_t>((payload[24] << 8) | payload[25]);
            entry.height = static_cast<uint32_t>((payload[26] << 8) | payload[27]);
            // VisualSampleEntry fixed fields are 78 bytes; children follow.
            size_t child_pos = 78;
            BoxView child;
            while (iterateBoxes(payload, payload_size, child_pos, child)) {
                if (child.type == fourcc('h', 'v', 'c', 'C')) {
                    entry.hvcc.assign(child.payload, child.payload + child.size);
                } else if (child.type == fourcc('e', 's', 'd', 's')) {
                    entry.esds.assign(child.payload, child.payload + child.size);
                }
            }
        } else if (is_audio) {
            const size_t child_offset = audioEntryChildOffset(payload, payload_size);
            if (child_offset == 0) {
                reader.skip(entry_size);
                continue;
            }
            entry.found = true;
            entry.format = format;
            entry.channels = static_cast<uint16_t>((payload[16] << 8) | payload[17]);
            entry.sample_rate = readU32(payload + 24) >> 16;
            size_t child_pos = child_offset;
            BoxView child;
while (iterateBoxes(payload, payload_size, child_pos, child)) {
                if (child.type == fourcc('e', 's', 'd', 's')) {
                    findDecoderSpecificInfo(child.payload, child.size, entry.codec_private);
                } else if (child.type == fourcc('d', 'O', 'p', 's')) {
                    // dOps is big endian while OpusHead is little endian, so
                    // every multi byte field has to be swapped. A verbatim copy
                    // turns a pre-skip of 312 into 14337 and drops the first
                    // quarter second of audio.
                    entry.codec_private = opusHeadFromDops(child.payload, child.size);
                } else if (child.type == fourcc('a', 'l', 'a', 'c')) {
                    entry.codec_private.assign(child.payload, child.payload + child.size);
                }
            }
        } else if (is_text) {
            entry.found = true;
            entry.format = format;
        }
        reader.skip(entry_size);
    }
    return true;
}

bool parseStbl(const BoxView& box, SampleEntry& entry, StblTables& tables) {
    size_t pos = 0;
    BoxView child;
    bool have_stsz = false;
    bool have_stts = false;
    bool have_stco = false;
    while (iterateBoxes(box.payload, box.size, pos, child)) {
        if (child.type == fourcc('s', 't', 's', 'd')) {
            if (!parseStsd(child, entry)) {
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

enum class TrackKind { Unknown, Video, Audio, Text };

struct TrackResult {
    TrackKind kind = TrackKind::Unknown;
    bool stbl_ok = false;
    uint32_t timescale = 0;
    uint64_t duration = 0;
    std::string language;
    uint32_t tkhd_width = 0;
    uint32_t tkhd_height = 0;
    SampleEntry entry;
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
                    if (!parseMdhd(media_child, result.timescale, result.duration,
                                   result.language)) {
                        return false;
                    }
                } else if (media_child.type == fourcc('h', 'd', 'l', 'r')) {
                    uint32_t handler = 0;
                    if (!parseHdlr(media_child, handler)) {
                        return false;
                    }
                    if (handler == fourcc('v', 'i', 'd', 'e')) {
                        result.kind = TrackKind::Video;
                    } else if (handler == fourcc('s', 'o', 'u', 'n')) {
                        result.kind = TrackKind::Audio;
                    } else if (handler == fourcc('s', 'b', 't', 'l') ||
                               handler == fourcc('t', 'e', 'x', 't') ||
                               handler == fourcc('s', 'u', 'b', 'p')) {
                        result.kind = TrackKind::Text;
                    }
                } else if (media_child.type == fourcc('m', 'i', 'n', 'f')) {
                    BoxView stbl;
                    if (findChild(media_child.payload, media_child.size, fourcc('s', 't', 'b', 'l'),
                                  stbl)) {
                        result.stbl_ok = parseStbl(stbl, result.entry, result.tables);
                    }
                }
            }
        }
    }
    return true;
}

bool buildSamples(const StblTables& tables, std::vector<Mp4Demuxer::SampleInfo>& samples,
                  std::string& error) {
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
        samples[i].duration = (i < tables.durations.size()) ? tables.durations[i] : 0;
        samples[i].keyframe = tables.has_stss ? tables.sync[i] : true;
    }
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
    bool have_video = false;

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
        if (track.kind == TrackKind::Video && track.entry.found && track.stbl_ok && !have_video) {
            video_track = std::move(track);
            have_video = true;
            continue;
        }
        if (track.kind == TrackKind::Audio && track.entry.found && track.stbl_ok) {
            TrackSamples packets;
            packets.timescale = track.timescale != 0 ? track.timescale : 1000;
            std::string track_error;
            if (!buildSamples(track.tables, packets.samples, track_error)) {
                continue;  // a track without usable tables is simply not listed
            }
            AudioTrackInfo info;
            info.index = audio_tracks_.size();
            info.language = track.language;
            info.codec = fourccText(track.entry.format);
            info.present = true;
            info.default_track = audio_tracks_.empty();
            info.codec_fourcc = track.entry.format;
            info.channels = track.entry.channels;
            info.sample_rate = track.entry.sample_rate;
            info.extradata = track.entry.codec_private;
            audio_tracks_.push_back(std::move(info));
            audio_samples_.push_back(std::move(packets));
            continue;
        }
        if (track.kind == TrackKind::Text && track.entry.found && track.stbl_ok) {
            TrackSamples packets;
            packets.timescale = track.timescale != 0 ? track.timescale : 1000;
            std::string track_error;
            if (!buildSamples(track.tables, packets.samples, track_error)) {
                continue;
            }
            SubtitleTrackInfo info;
            info.index = subtitle_tracks_.size();
            info.language = track.language;
            info.codec = track.entry.format == fourcc('t', 'x', '3', 'g') ? "tx3g" : "wvtt";
            subtitle_tracks_.push_back(std::move(info));
            subtitle_samples_.push_back(std::move(packets));
        }
    }

    if (!have_video) {
        error = "no HEVC or MPEG-1 video track found in MP4";
        return false;
    }

    // Decide between HEVC, MPEG-2 and MPEG-1 before reporting the track.
    Codec video_codec = Codec::Unknown;
    if (video_track.entry.format == fourcc('m', 'p', '4', 'v')) {
        EsdsInfo esds;
        std::string esds_error;
        if (!video_track.entry.esds.empty() &&
            parseEsds(video_track.entry.esds.data(), video_track.entry.esds.size(), esds,
                      &esds_error)) {
            if (esds.object_type == kObjectTypeMpeg1Video) {
                video_codec = Codec::Mpeg1;
            } else if (isMpeg2ObjectType(esds.object_type)) {
                video_codec = Codec::Mpeg2;
            } else if (esds.object_type == kObjectTypeMpeg4Visual ||
                       esds.object_type == kObjectTypeAvc) {
                error = "MPEG-4 Visual / H.264 video in MP4 is not supported";
                return false;
            } else {
                error = "unsupported video object type in esds";
                return false;
            }
        } else {
            error = esds_error.empty() ? "missing esds for mp4v track" : esds_error;
            return false;
        }
    } else {
        video_codec = Codec::Hevc;
    }

    video_ = VideoTrackInfo{};
    video_.codec = video_codec;
    video_.timescale = video_track.timescale != 0 ? video_track.timescale : 1000;
    video_.format.annexb = false;
    video_.format.nal_length_size = 4;
    video_.width = video_track.entry.width != 0 ? video_track.entry.width
                                                : video_track.tkhd_width;
    video_.height = video_track.entry.height != 0 ? video_track.entry.height
                                                 : video_track.tkhd_height;
    video_.hvcc = video_track.entry.hvcc;

    if (!video_.hvcc.empty()) {
        HvccInfo hvcc;
        std::string hvcc_error;
        if (parseHvcc(video_.hvcc.data(), video_.hvcc.size(), hvcc, &hvcc_error)) {
            video_.format.nal_length_size = hvcc.length_size;
        }
    }

    if (!buildSamples(video_track.tables, samples_, error)) {
        return false;
    }
    keyframes_.clear();
    for (size_t i = 0; i < samples_.size(); ++i) {
        if (samples_[i].keyframe) {
            keyframes_.emplace_back(samples_[i].pts, i);
        }
    }
    std::sort(keyframes_.begin(), keyframes_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

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

    // audio() stays the default track so single track callers keep working.
    if (!audio_tracks_.empty()) {
        audio_ = audio_tracks_.front();
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

bool Mp4Demuxer::selectAudioTrack(size_t index) {
    if (index >= audio_tracks_.size() || index >= audio_samples_.size()) {
        return false;
    }
    selected_audio_ = index;
    return true;
}

bool Mp4Demuxer::readAudioSample(Sample& sample) {
    if (selected_audio_ >= audio_samples_.size()) {
        return false;
    }
    TrackSamples& track = audio_samples_[selected_audio_];
    if (track.next >= track.samples.size()) {
        return false;
    }
    const SampleInfo& info = track.samples[track.next];
    sample.data.resize(info.size);
    if (info.size > 0 &&
        !readAt(file_, static_cast<int64_t>(info.offset), sample.data.data(), info.size)) {
        return false;
    }
    // The engine schedules audio in milliseconds.
    sample.pts = info.pts * 1000 / static_cast<int64_t>(track.timescale);
    sample.dts = info.dts * 1000 / static_cast<int64_t>(track.timescale);
    sample.keyframe = true;
    sample.track = TrackType::Audio;
    ++track.next;
    return true;
}

bool Mp4Demuxer::readSubtitleTrack(size_t index, std::vector<subtitle::SubtitleBlock>& blocks,
                                   std::string& error) {
    blocks.clear();
    if (index >= subtitle_samples_.size()) {
        error = "subtitle track index out of range";
        return false;
    }
    const TrackSamples& track = subtitle_samples_[index];
    if (track.samples.empty()) {
        error = "subtitle track has no samples";
        return false;
    }
    // A private handle: this runs on the UI thread while the pipeline reads
    // video samples through file_.
    FILE* file = std::fopen(path_.string().c_str(), "rb");
    if (!file) {
        error = "failed to reopen " + path_.string();
        return false;
    }
    bool ok = true;
    for (const SampleInfo& info : track.samples) {
        subtitle::SubtitleBlock block;
        block.data.resize(info.size);
        if (info.size > 0 &&
            (!readAt(file, static_cast<int64_t>(info.offset), block.data.data(), info.size))) {
            ok = false;
            break;
        }
        block.pts_ms = info.pts * 1000 / static_cast<int64_t>(track.timescale);
        block.duration_ms = info.duration * 1000 / static_cast<int64_t>(track.timescale);
        blocks.push_back(std::move(block));
    }
    std::fclose(file);
    if (!ok) {
        error = "failed to read subtitle sample data";
        return false;
    }
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

    // The audio cursors are independent of the video one, so they have to move
    // as well. Left ahead of the new playhead they hand the player packets from
    // the future, which the sink then refuses to play and playback goes silent
    // until the track is selected again.
    for (TrackSamples& track : audio_samples_) {
        const int64_t audio_target = static_cast<int64_t>(seconds * track.timescale);
        const auto first_after =
            std::lower_bound(track.samples.begin(), track.samples.end(), audio_target,
                             [](const SampleInfo& info, int64_t value) { return info.pts < value; });
        // Step back to the packet that covers the target for a short lead.
        track.next = first_after == track.samples.begin()
                         ? 0
                         : static_cast<size_t>(std::prev(first_after) - track.samples.begin());
    }
    return true;
}

}  // namespace player::demux
