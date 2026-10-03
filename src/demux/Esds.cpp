#include "demux/Esds.hpp"

#include <cstring>

namespace player::demux {
namespace {

// MPEG-4 descriptor tags used on the way to the DecoderConfigDescriptor.
constexpr uint8_t kTagEsDescriptor = 0x03;
constexpr uint8_t kTagDecoderConfigDescriptor = 0x04;
constexpr uint8_t kTagDecoderSpecificInfo = 0x05;

constexpr size_t kFixedHeader = 4;  // version, flags

class Cursor {
public:
    Cursor(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    bool has(size_t count) const { return offset_ + count <= size_; }

    uint8_t readU8() { return has(1) ? data_[offset_++] : 0; }

    // Reads a variable length descriptor size: seven bits per byte, high bit
    // means "continue".
    uint32_t readLength() {
        uint32_t length = 0;
        for (int i = 0; i < 4; ++i) {
            if (!has(1)) {
                return length;
            }
            const uint8_t byte = data_[offset_++];
            length = (length << 7) | static_cast<uint32_t>(byte & 0x7F);
            if ((byte & 0x80) == 0) {
                break;
            }
        }
        return length;
    }

    void skip(size_t count) {
        offset_ += count <= size_ - offset_ ? count : size_ - offset_;
    }

    size_t offset() const { return offset_; }
    void setOffset(size_t value) { offset_ = value <= size_ ? value : size_; }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t offset_ = 0;
};

// Walks the descriptor list, recording the decoder config and its optional
// DecoderSpecificInfo.
bool scanDescriptors(Cursor& cursor, size_t end, EsdsInfo& info) {
    while (cursor.offset() < end) {
        const uint8_t tag = cursor.readU8();
        const uint32_t length = cursor.readLength();
        const size_t body = cursor.offset();
        if (body + length > end) {
            return false;
        }

        switch (tag) {
            case kTagEsDescriptor:
                // Descend: the decoder config descriptor is nested inside.
                if (!scanDescriptors(cursor, body + length, info)) {
                    return false;
                }
                cursor.setOffset(body + length);
                break;
            case kTagDecoderConfigDescriptor: {
                // object_type_indication, stream_type, buffer_size, then the
                // optional maximum and average bitrates.
                if (!cursor.has(5)) {
                    return false;
                }
                info.object_type = cursor.readU8();
                const uint8_t stream_type_byte = cursor.readU8();
                info.stream_type = static_cast<uint8_t>(stream_type_byte >> 2);
                info.buffer_size = (static_cast<uint32_t>(cursor.readU8()) << 16) |
                                   (static_cast<uint32_t>(cursor.readU8()) << 8) |
                                   static_cast<uint32_t>(cursor.readU8());
                if (cursor.has(4)) {
                    cursor.skip(4);  // max_bitrate
                    cursor.skip(4);  // avg_bitrate
                }
                info.valid = info.object_type != 0;
                cursor.setOffset(body + length);
                // Descend into the nested descriptors.
                scanDescriptors(cursor, body + length, info);
                return info.valid;
            }
            case kTagDecoderSpecificInfo:
                // Decoder configuration is all the player needs; the
                // codec-specific payload is skipped.
                cursor.setOffset(body + length);
                break;
            default:
                cursor.setOffset(body + length);
                break;
        }
    }
    return info.valid;
}

}  // namespace

bool parseEsds(const uint8_t* data, size_t size, EsdsInfo& info, std::string* error) {
    info = EsdsInfo{};
    if (data == nullptr || size <= kFixedHeader) {
        if (error) {
            *error = "esds box is too small";
        }
        return false;
    }
    if (data[0] != 0) {
        if (error) {
            *error = "unsupported esds version";
        }
        return false;
    }

    Cursor cursor(data, size);
    cursor.skip(kFixedHeader);
    if (!scanDescriptors(cursor, size, info)) {
        if (error) {
            *error = "no decoder config descriptor in esds";
        }
        return false;
    }
    return true;
}

}  // namespace player::demux
