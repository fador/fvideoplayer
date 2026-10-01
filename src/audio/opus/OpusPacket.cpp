#include "audio/opus/OpusPacket.hpp"

namespace player::audio::opus {
namespace {

bool fail(std::string* error, const char* message) {
    if (error) {
        *error = message;
    }
    return false;
}

// Frame size codes: SILK 10/20/40/60 ms, hybrid 10/20 ms, CELT 2.5/5/10/20 ms.
constexpr uint16_t kSilkSamples[4] = {480, 960, 1920, 2880};
constexpr uint16_t kHybridSamples[2] = {480, 960};
constexpr uint16_t kCeltSamples[4] = {120, 240, 480, 960};

constexpr uint32_t kMaxPacketSamples = 5760;  // 120 ms at 48 kHz

// Reads a 1 or 2 byte frame size; returns the header bytes consumed or 0.
size_t readSize(const uint8_t* data, size_t size, size_t& value) {
    if (size < 1) {
        return 0;
    }
    if (data[0] < 252) {
        value = data[0];
        return 1;
    }
    if (size < 2) {
        return 0;
    }
    value = 4u * data[1] + data[0];
    return 2;
}

}  // namespace

uint16_t samplesPerFrame(uint8_t toc) {
    const uint8_t config = static_cast<uint8_t>(toc >> 3);
    if (config < 12) {
        return kSilkSamples[config & 3];
    }
    if (config < 16) {
        return kHybridSamples[config & 1];
    }
    return kCeltSamples[config & 3];
}

bool parsePacket(const uint8_t* data, size_t size, PacketInfo& info, std::string* error) {
    if (!data || size < 1) {
        return fail(error, "empty Opus packet");
    }
    const uint8_t toc = data[0];
    const uint8_t config = static_cast<uint8_t>(toc >> 3);

    if (config < 12) {
        info.mode = Mode::Silk;
        info.bandwidth = config < 4 ? Bandwidth::Narrowband
                                    : (config < 8 ? Bandwidth::Mediumband : Bandwidth::Wideband);
    } else if (config < 16) {
        info.mode = Mode::Hybrid;
        info.bandwidth = config < 14 ? Bandwidth::SuperWideband : Bandwidth::Fullband;
    } else {
        info.mode = Mode::Celt;
        info.bandwidth = config < 20 ? Bandwidth::Narrowband
                                     : (config < 24 ? Bandwidth::Wideband
                                                    : (config < 28 ? Bandwidth::SuperWideband
                                                                   : Bandwidth::Fullband));
    }
    info.stereo = (toc & 0x04) != 0;
    info.frame_samples = samplesPerFrame(toc);

    //    Opus packet layout (RFC 6716 3.2): TOC, optional frame count and
    //    sizes, frames, optional padding at the end.
    const size_t code = toc & 0x03;
    size_t cursor = 1;
    size_t frames_end = size;
    size_t frame_count = 1;
    size_t sizes[kMaxFramesPerPacket] = {};

    if (code == 0) {
        frame_count = 1;
    } else if (code == 1) {
        if ((size - 1) % 2 != 0) {
            return fail(error, "invalid Opus CBR packet size");
        }
        frame_count = 2;
        sizes[0] = (size - 1) / 2;
    } else if (code == 2) {
        size_t first = 0;
        const size_t consumed = readSize(data + cursor, size - cursor, first);
        if (consumed == 0 || first > size - cursor - consumed) {
            return fail(error, "invalid Opus frame size");
        }
        cursor += consumed;
        frame_count = 2;
        sizes[0] = first;
    } else {
        if (cursor >= size) {
            return fail(error, "truncated Opus frame count");
        }
        const uint8_t count_byte = data[cursor++];
        frame_count = count_byte & 0x3F;
        if (frame_count == 0 ||
            static_cast<uint32_t>(info.frame_samples) * frame_count > kMaxPacketSamples) {
            return fail(error, "invalid Opus frame count");
        }

        if ((count_byte & 0x40) != 0) {  // padding lives at the end of the packet
            size_t padding = 0;
            for (;;) {
                if (cursor >= size) {
                    return fail(error, "truncated Opus padding");
                }
                const uint8_t length_byte = data[cursor++];
                padding += length_byte == 255 ? 254 : length_byte;
                if (length_byte != 255) {
                    break;
                }
            }
            if (padding > size - cursor) {
                return fail(error, "invalid Opus padding");
            }
            frames_end = size - padding;
        }

        if ((count_byte & 0x80) != 0) {  // VBR: explicit sizes for all but the last
            for (size_t i = 0; i + 1 < frame_count; ++i) {
                size_t frame_size = 0;
                const size_t consumed = readSize(data + cursor, frames_end - cursor, frame_size);
                if (consumed == 0 || frame_size > frames_end - cursor - consumed) {
                    return fail(error, "invalid Opus frame size");
                }
                cursor += consumed;
                sizes[i] = frame_size;
            }
        } else {  // CBR: equal frames
            const size_t body = frames_end - cursor;
            if (body % frame_count != 0) {
                return fail(error, "invalid Opus CBR packet size");
            }
            for (size_t i = 0; i + 1 < frame_count; ++i) {
                sizes[i] = body / frame_count;
            }
        }
    }

    size_t used = 0;
    for (size_t i = 0; i + 1 < frame_count; ++i) {
        used += sizes[i];
    }
    if (frames_end < cursor || used > frames_end - cursor) {
        return fail(error, "invalid Opus packet length");
    }
    sizes[frame_count - 1] = frames_end - cursor - used;

    info.header_size = cursor;
    info.frame_count = static_cast<uint8_t>(frame_count);
    for (size_t i = 0; i < frame_count; ++i) {
        info.frames[i].offset = cursor;
        info.frames[i].size = sizes[i];
        cursor += sizes[i];
    }
    if (cursor != frames_end) {
        return fail(error, "Opus packet size mismatch");
    }
    return true;
}

}  // namespace player::audio::opus
