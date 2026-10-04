#include "demux/MpegEsScan.hpp"

namespace player::demux {
namespace {

constexpr uint8_t kStartSequence = 0xB3;
constexpr uint8_t kStartExtension = 0xB5;
constexpr uint8_t kStartGop = 0xB8;
constexpr uint8_t kStartUserData = 0xB2;
constexpr uint8_t kStartSequenceEnd = 0xB7;
constexpr uint8_t kStartPicture = 0x00;

// frame_rate_code (ISO/IEC 11172-2 table 2-4) to frames per second. Index 0
// and 9..15 are forbidden or reserved, and report 0 so the caller can fall
// back to something else.
double frameRateFromCode(uint32_t code) {
    switch (code) {
        case 1:
            return 24000.0 / 1001.0;
        case 2:
            return 24.0;
        case 3:
            return 25.0;
        case 4:
            return 30000.0 / 1001.0;
        case 5:
            return 30.0;
        case 6:
            return 50.0;
        case 7:
            return 60000.0 / 1001.0;
        case 8:
            return 60.0;
        default:
            return 0.0;
    }
}

}  // namespace

bool findMpegStartCode(const std::vector<uint8_t>& buffer, size_t from, MpegStartCode& out) {
    for (size_t i = from; i + 3 < buffer.size(); ++i) {
        if (buffer[i] != 0 || buffer[i + 1] != 0 || buffer[i + 2] != 1) {
            continue;
        }
        size_t start = i;
        while (start > 0 && buffer[start - 1] == 0) {
            --start;
        }
        out.run_start = static_cast<int64_t>(start);
        out.header = static_cast<int64_t>(i);
        out.body = static_cast<int64_t>(i) + 4;
        out.code = buffer[i + 3];
        return true;
    }
    return false;
}

uint8_t parseMpegPictureType(const std::vector<uint8_t>& buffer, int64_t offset, int64_t limit) {
    if (offset + 4 > limit) {
        return 1;
    }
    // temporal_reference is 10 bits, then the 3-bit picture_coding_type.
    const uint32_t bits = (static_cast<uint32_t>(buffer[static_cast<size_t>(offset)]) << 24) |
                          (static_cast<uint32_t>(buffer[static_cast<size_t>(offset) + 1]) << 16) |
                          (static_cast<uint32_t>(buffer[static_cast<size_t>(offset) + 2]) << 8) |
                          static_cast<uint32_t>(buffer[static_cast<size_t>(offset) + 3]);
    const uint8_t type = static_cast<uint8_t>((bits >> 19) & 0x07);
    return type >= 1 && type <= 3 ? type : 1;
}

void scanMpegPictures(const std::vector<uint8_t>& buffer, int64_t file_size,
                      std::vector<MpegEsPicture>& pictures, MpegEsInfo& info) {
    MpegStartCode sc;
    if (!findMpegStartCode(buffer, 0, sc)) {
        return;
    }

    int64_t picture_start = -1;
    // Start of the sequence or group header block that has not been emitted yet.
    int64_t header_start = -1;
    int type = 1;
    bool awaiting_extension = false;

    for (;;) {
        const int64_t body = sc.body;
        const uint8_t code = sc.code;

        if (code == kStartSequence || code == kStartGop) {
            // A sequence or group header introduces the picture that follows
            // it, so it ends any picture still open and becomes the start of
            // the next one. Folding it into the following picture is what lets
            // a decoder start at any sample.
            if (picture_start >= 0) {
                pictures.push_back({picture_start, sc.header - picture_start, type, 0, false});
                picture_start = -1;
            }
            header_start = header_start >= 0 ? header_start : sc.header;
            awaiting_extension = code == kStartSequence;

            if (code == kStartSequence && body + 4 <= static_cast<int64_t>(buffer.size())) {
                // The first four bytes carry horizontal_size, vertical_size,
                // pixel_aspect_ratio and frame_rate_code.
                const size_t at = static_cast<size_t>(body);
                const uint32_t bits =
                    (static_cast<uint32_t>(buffer[at]) << 24) |
                    (static_cast<uint32_t>(buffer[at + 1]) << 16) |
                    (static_cast<uint32_t>(buffer[at + 2]) << 8) |
                    static_cast<uint32_t>(buffer[at + 3]);
                info.width = (bits >> 20) & 0x0FFF;
                info.height = (bits >> 8) & 0x0FFF;
                info.fps = frameRateFromCode(bits & 0x0F);
            }
        } else if (code == kStartExtension && awaiting_extension) {
            // A sequence extension (extension id 1) is what makes it MPEG-2.
            if (body < static_cast<int64_t>(buffer.size()) &&
                (buffer[static_cast<size_t>(body)] >> 4) == 1) {
                info.mpeg2 = true;
            }
            awaiting_extension = false;
        } else if (code == kStartPicture) {
            awaiting_extension = false;
            if (picture_start >= 0) {
                pictures.push_back({picture_start, sc.header - picture_start, type, 0, false});
            }
            // Carry any not-yet-emitted sequence or group header along.
            picture_start = header_start >= 0 ? header_start : sc.header;
            type = parseMpegPictureType(buffer, body, file_size);
            header_start = -1;
        } else if (code == kStartSequenceEnd) {
            if (picture_start >= 0) {
                pictures.push_back({picture_start, sc.header - picture_start, type, 0, false});
            }
            picture_start = -1;
            break;
        } else if (picture_start < 0 && code == kStartUserData) {
            header_start = header_start >= 0 ? header_start : sc.header;
        }

        if (!findMpegStartCode(buffer, static_cast<size_t>(sc.body), sc)) {
            break;
        }
    }

    if (picture_start >= 0 && file_size > picture_start) {
        pictures.push_back({picture_start, file_size - picture_start, type, 0, false});
    }
    info.pictures = static_cast<int>(pictures.size());
}

}  // namespace player::demux
