#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace player::demux {

// Shared elementary stream scanning for the raw MPEG and program stream
// demuxers. Both end up with a buffer of start-code prefixed MPEG-1/MPEG-2
// video and need the same picture splitting and sequence header parsing.

struct MpegEsPicture {
    int64_t offset = 0;
    int64_t size = 0;
    int type = 0;      // picture_coding_type, 1 = I
    int64_t pts_ms = 0;
    bool has_pts = false;
    // Group index and temporal_reference, the picture's display position
    // inside that group.
    int gop = 0;
    int temporal_reference = 0;
    // Position in display order, counted from the start of the stream. The
    // pictures themselves are stored in decode order, so a timeline built by
    // stepping this through the frame rate stays monotonic where one built
    // from the decode order would not.
    int display_index = 0;
};

struct MpegEsInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    bool mpeg2 = false;
    int pictures = 0;
    // Frame rate from the sequence header's frame_rate_code, scaled by the
    // sequence extension when there is one. 0 when no sequence header was seen.
    double fps = 0.0;
};

struct MpegStartCode {
    int64_t run_start = 0;  // first byte of the leading zero run
    int64_t header = 0;     // offset of the `00 00 01` pattern itself
    int64_t body = 0;       // first byte after the code byte
    uint8_t code = 0;
};

// Finds the next `00 00 01` prefix at or after `from`.
//
// `run_start` and `header` differ when the preceding element ends in zero
// padding: those zeros are ambiguous with the start code's own leading zeros,
// and the convention here is that they belong to the preceding element. So an
// element ends at the *next* start code's `run_start` and begins at its own
// `header`.
bool findMpegStartCode(const std::vector<uint8_t>& buffer, size_t from, MpegStartCode& out);

// picture_coding_type of the picture header whose payload starts at `offset`.
uint8_t parseMpegPictureType(const std::vector<uint8_t>& buffer, int64_t offset, int64_t limit);

// Splits the stream into pictures at picture headers. A picture runs from its
// own picture header to the next one, and the sequence and group headers that
// precede a picture are folded into that picture's sample, because the decoder
// needs them and there is no separate sample for them.
//
// Also reports the geometry from the sequence header, whether a sequence
// extension was seen (which is what makes the stream MPEG-2), and fills in
// each picture's type.
void scanMpegPictures(const std::vector<uint8_t>& buffer, int64_t file_size,
                      std::vector<MpegEsPicture>& pictures, MpegEsInfo& info);

}  // namespace player::demux
