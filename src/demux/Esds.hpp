#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace player::demux {

// objectTypeIndication values of the MPEG-4 systems DecoderConfigDescriptor
// (ISO/IEC 14496-1) for the video codecs the player knows about.
constexpr uint8_t kObjectTypeMpeg1Video = 0x01;
constexpr uint8_t kObjectTypeMpeg4Visual = 0x20;
constexpr uint8_t kObjectTypeAvc = 0x21;
// MPEG-2 video profiles, 0x60 (Simple) through 0x65 (4:2:2).
constexpr uint8_t kObjectTypeMpeg2SimpleVideo = 0x60;
constexpr uint8_t kObjectTypeMpeg2MainVideo = 0x61;
constexpr uint8_t kObjectTypeMpeg2SnrVideo = 0x62;
constexpr uint8_t kObjectTypeMpeg2SpatialVideo = 0x63;
constexpr uint8_t kObjectTypeMpeg2HighVideo = 0x64;
constexpr uint8_t kObjectTypeMpeg2_422Video = 0x65;

// True for the object types that carry H.262 video.
inline bool isMpeg2ObjectType(uint8_t object_type) {
    return object_type >= kObjectTypeMpeg2SimpleVideo && object_type <= kObjectTypeMpeg2_422Video;
}

struct EsdsInfo {
    bool valid = false;
    uint8_t object_type = 0;
    uint8_t stream_type = 0;   // upper 6 bits of the same byte
    uint32_t buffer_size = 0;  // the declared buffer size, not the bitrate
};

// Parses an MPEG-4 ES descriptor box and extracts the video object type of the
// elementary stream. The parser only needs to descend to the
// DecoderConfigDescriptor, so unknown descriptors are skipped by length.
bool parseEsds(const uint8_t* data, size_t size, EsdsInfo& info, std::string* error);

}  // namespace player::demux
